#include "app_ui.h"

#include "app_config.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "lvgl.h"
#include "ma_client.h"
#include "ota_server.h"
#include "provisioning.h"
#include "voice_client.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ma_remote";

#define INPUT_QUEUE_DEPTH 8
#define CMD_QUEUE_DEPTH 8
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
#define DIM_AFTER_US (30LL * 1000 * 1000)
#define DEEP_SLEEP_AFTER_US (10LL * 60 * 1000 * 1000)
#define MIN_SCREEN_OFF_S 35

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

typedef enum {
    APP_CMD_PLAY_PAUSE = 0,
    APP_CMD_NEXT,
    APP_CMD_PREVIOUS,
    APP_CMD_VOLUME_UP,
    APP_CMD_VOLUME_DOWN,
} app_cmd_t;

static app_config_t s_config;
static QueueHandle_t s_input_queue;
static QueueHandle_t s_cmd_queue;
static SemaphoreHandle_t s_queue_mutex;
static ma_players_t s_players;
static char s_queue_id[96];
static volatile bool s_input_ready;
static volatile bool s_is_playing;
static volatile bool s_volume_known;
static int s_volume_feedback = -1;
static bool s_volume_feedback_valid;
static volatile int64_t s_last_activity_us;
static volatile bool s_display_on = true;
static volatile bool s_display_dim;
static volatile bool s_sleeping;
static volatile bool s_resolve_player = true;
static int s_suppress_btn = -1;
static key_guard_t s_key_guard;
static int s_volume = 50;
static bool s_volume_target_valid;
static int64_t s_ok_press_us;
static bool s_ptt_active;
static bool s_ptt_long;
static int64_t s_ptt_short_until_us;
static int64_t s_ptt_commit_until_us;

static void ui_wifi(bool connected);
static void ui_message(const char *headline, const char *detail);
static void ui_track(const ma_status_t *st);
static bool queue_id_copy(char *out, size_t cap);

// Runs on the button component's shared esp_timer task: enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    s_last_activity_us = esp_timer_get_time();
    // A release can arrive after the post-init voltage sample but before input
    // dispatch is ready; run the guard first so that release still disarms it.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "wake key released, buttons active again");
        }
        return;
    }
    // While the screen is off, keys only wake the display; swallow the whole
    // gesture so the press that woke it cannot change playback or volume.
    if (!s_display_on) {
        s_suppress_btn = (int)btn;
        if (ev == BSP_BTN_RELEASE) s_suppress_btn = -1;
        return;
    }
    if (s_suppress_btn == (int)btn) {
        if (ev == BSP_BTN_RELEASE) s_suppress_btn = -1;
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void enqueue_cmd(app_cmd_t cmd) {
    if (s_cmd_queue) (void)xQueueSend(s_cmd_queue, &cmd, 0);
}

static void handle_input(const input_event_t *input) {
    if (input->btn == BSP_BTN_OK) {
        const int64_t now = esp_timer_get_time();
        if (input->ev == BSP_BTN_PRESS) {
            if (voice_client_busy()) return;
            s_ok_press_us = now;
            s_ptt_active = true;
            s_ptt_long = false;
            char qid[96];
            const bool have_queue = queue_id_copy(qid, sizeof(qid));
            voice_client_start(s_config.player_id, have_queue ? qid : "");
            return;
        }
        if (input->ev == BSP_BTN_RELEASE) {
            if (!s_ptt_active) return;
            s_ptt_active = false;
            const int64_t held = now - s_ok_press_us;
            const bool long_press =
                s_ptt_long || held >= (int64_t)BSP_BTN_LONG_PRESS_MS * 1000;
            s_ptt_long = false;
            if (long_press) {
                s_ptt_commit_until_us = now;
                voice_client_commit();
                voice_client_stop();
            } else {
                s_ptt_short_until_us = now + 1000000;
                voice_client_cancel();
            }
            return;
        }
        if (input->ev == BSP_BTN_LONG) {
            if (!s_ptt_active || s_ptt_long) return;
            s_ptt_long = true;
            voice_client_listening();
            return;
        }
        if (input->ev == BSP_BTN_CLICK) {
            if (s_ptt_short_until_us && now < s_ptt_short_until_us) {
                s_ptt_short_until_us = 0;
                enqueue_cmd(APP_CMD_PLAY_PAUSE);
                return;
            }
            if (s_ptt_commit_until_us && now - s_ptt_commit_until_us < 1000000) {
                s_ptt_commit_until_us = 0;
                return;
            }
            if (voice_client_busy()) return;
            enqueue_cmd(APP_CMD_PLAY_PAUSE);
            return;
        }
        return;
    }

    if (input->ev == BSP_BTN_LONG) {
        if (input->btn == BSP_BTN_UP) enqueue_cmd(APP_CMD_PREVIOUS);
        else if (input->btn == BSP_BTN_DOWN) enqueue_cmd(APP_CMD_NEXT);
        return;
    }

    if (input->ev != BSP_BTN_CLICK) return;

    switch (input->btn) {
    case BSP_BTN_UP:
        enqueue_cmd(APP_CMD_VOLUME_UP);
        break;
    case BSP_BTN_DOWN:
        enqueue_cmd(APP_CMD_VOLUME_DOWN);
        break;
    default:
        break;
    }
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        if (!bsp_lvgl_lock(250)) continue;
        handle_input(&input);
        bsp_lvgl_unlock();
    }
}

static void ui_playing(bool playing) {
    if (bsp_lvgl_lock(200)) {
        app_ui_set_playing(playing);
        bsp_lvgl_unlock();
    }
}

static void ui_volume_ex(int volume, bool estimated) {
    if (bsp_lvgl_lock(200)) {
        app_ui_set_volume_ex(volume, estimated);
        bsp_lvgl_unlock();
    }
}

static void ui_volume(int volume) {
    ui_volume_ex(volume, false);
}

static int normal_brightness(void) {
    return s_config.brightness ? s_config.brightness : 100;
}

static void display_wake(void) {
    const bool was_off = !s_display_on;
    s_display_on = true;
    s_display_dim = false;
    bsp_display_backlight((uint8_t)normal_brightness());
    wifi_sta_set_power_save(false);
    if (was_off) {
        s_resolve_player = true;
        ESP_LOGI(TAG, "wake: display on, polling resumed");
    }
}

static void display_dim(void) {
    if (!s_display_on || s_display_dim) return;
    s_display_dim = true;
    int dim = normal_brightness() * 20 / 100;
    if (dim < 5) dim = 5;
    bsp_display_backlight((uint8_t)dim);
    ESP_LOGI(TAG, "idle: display dimmed");
}

static void display_off(void) {
    if (!s_display_on) return;
    s_display_on = false;
    s_display_dim = false;
    bsp_display_backlight(0);
    wifi_sta_set_power_save(true);
    ESP_LOGI(TAG, "idle: display off, polling paused");
}

static void enter_deep_sleep(int64_t idle_us) {
    if (usb_serial_jtag_is_connected()) {
        ESP_LOGI(TAG, "USB host connected: deep sleep skipped");
        s_last_activity_us = esp_timer_get_time();
        return;
    }
    s_sleeping = true;
    ESP_LOGW(TAG, "idle %llds: entering deep sleep", (long long)(idle_us / 1000000LL));

    const int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "key held (%d mV), sleep postponed", mv);
        s_sleeping = false;
        s_last_activity_us = esp_timer_get_time();
        return;
    }

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    int level = 0;
    esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button deep-sleep handoff failed: %s", esp_err_to_name(err));
        esp_restart();
    }
    err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO wake arm failed: %s", esp_err_to_name(err));
        esp_restart();
    }

    err = bsp_battery_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "battery sleep failed: %s", esp_err_to_name(err));
    err = bsp_audio_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "audio sleep failed: %s", esp_err_to_name(err));
    err = bsp_audio_prepare_deep_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "audio pin release failed: %s", esp_err_to_name(err));
    err = bsp_i2c_prepare_deep_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "I2C release failed: %s", esp_err_to_name(err));
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock failed before display sleep");
        esp_restart();
    }
    err = bsp_display_prepare_deep_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "display sleep failed: %s", esp_err_to_name(err));

    esp_deep_sleep_start();
    ESP_LOGE(TAG, "deep sleep returned unexpectedly");
    esp_restart();
}

static void power_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_sleeping) continue;
        if (ota_server_busy()) {
            s_last_activity_us = esp_timer_get_time();
            if (!s_display_on) display_wake();
            continue;
        }
        const int64_t idle = esp_timer_get_time() - s_last_activity_us;
        int off_s = s_config.sleep_timeout_s;
        if (off_s < MIN_SCREEN_OFF_S) off_s = MIN_SCREEN_OFF_S;
        const int64_t off_us = (int64_t)off_s * 1000000LL;

        if (s_display_on) {
            if (idle >= DEEP_SLEEP_AFTER_US) {
                display_off();
                enter_deep_sleep(idle);
            } else if (idle >= off_us) {
                display_off();
            } else if (idle >= DIM_AFTER_US) {
                display_dim();
            } else if (s_display_dim) {
                display_wake();
            }
        } else if (idle >= DEEP_SLEEP_AFTER_US) {
            enter_deep_sleep(idle);
        } else if (idle < off_us) {
            display_wake();
        }
    }
}

static void queue_id_store(const char *id) {
    if (!s_queue_mutex) return;
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        snprintf(s_queue_id, sizeof(s_queue_id), "%s", id);
        xSemaphoreGive(s_queue_mutex);
    }
}

static bool queue_id_copy(char *out, size_t cap) {
    bool ok = false;
    if (!s_queue_mutex) return false;
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_queue_id[0]) {
            snprintf(out, cap, "%s", s_queue_id);
            ok = true;
        }
        xSemaphoreGive(s_queue_mutex);
    }
    return ok;
}

static void resolve_player(void) {
    if (ma_fetch_players(&s_players) != ESP_OK) {
        ESP_LOGW(TAG, "active player resolution failed (heap %u)",
                 esp_get_free_heap_size());
        return;
    }

    const ma_player_t *playing_config = NULL;
    const ma_player_t *playing_any = NULL;
    const ma_player_t *group_media = NULL;
    const ma_player_t *paused_config = NULL;
    const ma_player_t *paused_any = NULL;
    const ma_player_t *configured = NULL;
    for (int i = 0; i < s_players.count; i++) {
        const ma_player_t *player = &s_players.items[i];
        const bool is_config = strcmp(player->player_id, s_config.player_id) == 0;
        if (is_config) configured = player;
        if (player->is_group && player->has_media && !group_media) {
            group_media = player;
        }
        if (strcmp(player->state, "playing") == 0) {
            if (is_config) playing_config = player;
            else if (!playing_any) playing_any = player;
        } else if (strcmp(player->state, "paused") == 0 && player->has_media) {
            if (is_config) paused_config = player;
            else if (!paused_any) paused_any = player;
        }
    }

    const ma_player_t *best = group_media;
    if (!best) best = playing_config ? playing_config : playing_any;
    if (!best) best = paused_config ? paused_config : paused_any;
    if (!best) best = configured;
    if (!best && s_players.count > 0) best = &s_players.items[0];
    if (!best) return;
    if (strcmp(best->player_id, s_config.player_id) == 0) return;

    ESP_LOGI(TAG, "MA active player -> %s (%s, %s)", best->display, best->player_id,
             best->state);
    snprintf(s_config.player_id, sizeof(s_config.player_id), "%s", best->player_id);
    ma_client_set_player(s_config.player_id);
    const esp_err_t save = app_config_save_player(s_config.player_id);
    if (save != ESP_OK) {
        ESP_LOGW(TAG, "save active player failed: %s", esp_err_to_name(save));
    }
    queue_id_store("");
    s_volume_known = false;
    s_volume_target_valid = false;
    s_volume_feedback_valid = false;
    s_volume_feedback = -1;
    ui_volume(-1);
    s_is_playing = false;

    char qid[96];
    if (ma_fetch_queue_id(qid, sizeof(qid)) == ESP_OK) {
        queue_id_store(qid);
    } else {
        ESP_LOGW(TAG, "active player queue lookup failed");
    }
}

static void cmd_task(void *arg) {
    (void)arg;
    app_cmd_t cmd;
    char qid[96];
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd) {
        case APP_CMD_PLAY_PAUSE: {
            const bool target = !s_is_playing;
            s_is_playing = target;
            ui_playing(target);
            ESP_LOGI(TAG, "cmd play_pause -> %s", target ? "play" : "pause");
            if (ma_play_pause() != ESP_OK) {
                s_is_playing = !target;
                ui_playing(!target);
            }
            break;
        }
        case APP_CMD_VOLUME_UP:
        case APP_CMD_VOLUME_DOWN:
            if (!s_volume_target_valid) {
                // Some players (notably groups) never report a volume. Step
                // from our own target instead of MA's volume_up/down, which
                // restarts at 0 when the player reports nothing.
                s_volume = 0;
                s_volume_target_valid = true;
            }
            if (cmd == APP_CMD_VOLUME_UP) {
                if (s_volume <= 95) s_volume += 5;
                else s_volume = 100;
            } else {
                if (s_volume >= 5) s_volume -= 5;
                else s_volume = 0;
            }
            ui_volume_ex(s_volume, !s_volume_known);
            ESP_LOGI(TAG, "cmd volume_set %d (feedback=%s)", s_volume,
                     s_volume_known ? "known" : "none");
            (void)ma_volume_set(s_volume);
            break;
        case APP_CMD_NEXT:
        case APP_CMD_PREVIOUS:
            qid[0] = '\0';
            if (!queue_id_copy(qid, sizeof(qid)) &&
                ma_fetch_queue_id(qid, sizeof(qid)) == ESP_OK) {
                queue_id_store(qid);
            }
            if (!qid[0]) {
                ESP_LOGW(TAG, "queue id unavailable");
                break;
            }
            ESP_LOGI(TAG, "cmd %s queue=%s",
                     cmd == APP_CMD_NEXT ? "next" : "previous", qid);
            if (cmd == APP_CMD_NEXT) {
                (void)ma_queue_next(qid);
            } else {
                (void)ma_queue_previous(qid);
            }
            break;
        }
    }
}

static void ui_wifi(bool connected) {
    if (bsp_lvgl_lock(200)) {
        app_ui_set_wifi(connected);
        bsp_lvgl_unlock();
    }
}

static void ui_message(const char *headline, const char *detail) {
    if (bsp_lvgl_lock(250)) {
        app_ui_set_message(headline, detail);
        bsp_lvgl_unlock();
    }
}

static void ui_track(const ma_status_t *st) {
    char subtitle[200];
    if (st->artist[0] && st->album[0]) {
        snprintf(subtitle, sizeof(subtitle), "%s · %s", st->artist, st->album);
    } else {
        snprintf(subtitle, sizeof(subtitle), "%s%s", st->artist, st->album);
    }
    if (bsp_lvgl_lock(250)) {
        app_ui_set_track(st->title, subtitle, st->player_name, st->playing,
                         st->elapsed_s, st->duration_s);
        bsp_lvgl_unlock();
    }
}

static void net_task(void *arg) {
    (void)arg;
    wifi_sta_state_t last_wifi = WIFI_STA_IDLE;
    while (wifi_sta_state() != WIFI_STA_CONNECTED) {
        const wifi_sta_state_t state = wifi_sta_state();
        if (state != last_wifi) {
            ui_wifi(state == WIFI_STA_CONNECTED);
            last_wifi = state;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ui_wifi(true);
    last_wifi = WIFI_STA_CONNECTED;
    const esp_err_t ota_err = ota_server_start();
    if (ota_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA server unavailable: %s", esp_err_to_name(ota_err));
    }
    ui_message("正在连接", "Music Assistant");

    if (ma_client_init(&s_config) != ESP_OK) {
        ui_message("配置缺失", "重新配网或填写 token");
        vTaskDelete(NULL);
        return;
    }

    char queue_id[96];
    bool queue_ok =
        s_config.player_id[0] &&
        ma_fetch_queue_id(queue_id, sizeof(queue_id)) == ESP_OK;
    if (!queue_ok && s_config.player_id[0]) {
        vTaskDelay(pdMS_TO_TICKS(250));
        queue_ok = ma_fetch_queue_id(queue_id, sizeof(queue_id)) == ESP_OK;
    }
    if (queue_ok) {
        queue_id_store(queue_id);
        ESP_LOGI(TAG, "using queue '%s', free heap %u", queue_id, esp_get_free_heap_size());
    } else {
        ESP_LOGW(TAG, "active queue lookup failed");
    }

    bool auth_failed = false;
    bool ma_offline = false;
    char last_key[256] = "";
    int64_t last_heartbeat_us = 0;
    for (;;) {
        const wifi_sta_state_t state = wifi_sta_state();
        if (state != last_wifi) {
            ui_wifi(state == WIFI_STA_CONNECTED);
            if (state != WIFI_STA_CONNECTED && !voice_client_ui_blocked() &&
                !ota_server_ui_blocked()) {
                ui_message("WiFi 断开", "重连中");
            } else if (state == WIFI_STA_CONNECTED && last_wifi != WIFI_STA_IDLE) {
                s_resolve_player = true;
            }
            last_wifi = state;
        }
        if (state != WIFI_STA_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_display_on) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (ota_server_busy()) {
            // OTA owns the screen and the network while it writes Flash.
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_resolve_player) {
            s_resolve_player = false;
            ma_status_t probe;
            const bool keep_current =
                s_config.player_id[0] && ma_fetch_status(&probe) == ESP_OK &&
                (probe.playing || probe.title[0] || probe.duration_s > 0);
            if (!keep_current) resolve_player();
        }
        if (!s_config.player_id[0]) {
            if (!ota_server_ui_blocked()) ui_message("没有可用播放器", "检查 MA");
            s_resolve_player = true;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (auth_failed) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        ma_status_t st;
        const esp_err_t err = ma_fetch_status(&st);
        if (err == ESP_OK) {
            s_is_playing = st.playing;
            if (!st.volume_known) {
                if (s_volume_known) {
                    s_volume_known = false;
                    if (s_volume_target_valid) ui_volume_ex(s_volume, true);
                    else ui_volume(-1);
                }
            } else if (!s_volume_feedback_valid || st.volume != s_volume_feedback) {
                // Accept real feedback, including changes made outside the
                // remote. Unchanged feedback is not allowed to overwrite an
                // optimistic target: some players keep reporting 0 forever.
                s_volume_feedback = st.volume;
                s_volume_feedback_valid = true;
                s_volume_known = true;
                s_volume_target_valid = true;
                s_volume = st.volume;
                ui_volume_ex(s_volume, false);
            } else if (!s_volume_target_valid) {
                s_volume_target_valid = true;
                s_volume_known = true;
                s_volume = st.volume;
                ui_volume_ex(s_volume, false);
            } else if (st.volume == s_volume) {
                if (!s_volume_known) {
                    s_volume_known = true;
                    ui_volume_ex(s_volume, false);
                }
            } else if (s_volume_known) {
                s_volume_known = false;
                ui_volume_ex(s_volume, true);
            }
            if (!voice_client_ui_blocked() && !ota_server_ui_blocked()) {
                ui_wifi(true);
                ui_track(&st);
            }
            ma_offline = false;
            char key[256];
            snprintf(key, sizeof(key), "%s|%s|%s", st.title, st.artist, st.state);
            if (strcmp(key, last_key) != 0) {
                snprintf(last_key, sizeof(last_key), "%s", key);
                ESP_LOGI(TAG, "state=%s %d/%ds title='%s' artist='%s' player='%s'",
                         st.state, st.elapsed_s, st.duration_s, st.title, st.artist,
                         st.player_name);
            }
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_heartbeat_us > 30LL * 1000 * 1000) {
                last_heartbeat_us = now_us;
                ESP_LOGI(TAG, "poll ok: %s %d/%ds heap=%u", st.state, st.elapsed_s,
                         st.duration_s, esp_get_free_heap_size());
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (err == ESP_ERR_INVALID_STATE) {
            auth_failed = true;
            if (!voice_client_ui_blocked() && !ota_server_ui_blocked()) {
                ui_message("Token 失效", "更新 MA token");
            }
            ESP_LOGE(TAG, "MA rejected token (HTTP %d)", ma_last_http_status());
        } else {
            ui_wifi(true);
            if (!ma_offline) {
                ma_offline = true;
                if (!voice_client_ui_blocked() && !ota_server_ui_blocked()) {
                    ui_message("MA 离线", "重试中");
                }
            }
            ESP_LOGW(TAG, "players/get failed (HTTP %d)", ma_last_http_status());
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "AI Passport MA remote M4 starting");

    esp_err_t config_err = app_config_load(&s_config);
    if (config_err != ESP_OK) {
        ESP_LOGW(TAG, "config load failed: %s (using defaults)",
                 esp_err_to_name(config_err));
    }

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "key wake: swallowing key events until released");
    }

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed");
        return;
    }
    bsp_display_backlight(s_config.brightness ? s_config.brightness : 100);

    esp_err_t battery_err = bsp_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge init failed: %s", esp_err_to_name(battery_err));
    }

    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        app_ui_set_card_opacity(s_config.card_opacity);
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock failed while building UI");
        return;
    }

    if (!s_config.provisioned || !s_config.wifi_ssid[0] || !s_config.ma_host[0] ||
        !s_config.bridge_host[0]) {
        provisioning_run(&s_config);
    }

    esp_err_t voice_err = voice_client_init(&s_config);
    if (voice_err != ESP_OK) {
        ESP_LOGW(TAG, "voice init failed: %s", esp_err_to_name(voice_err));
    }

    s_queue_mutex = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(app_cmd_t));
    if (s_queue_mutex && s_cmd_queue) {
        if (xTaskCreate(cmd_task, "app_cmd", 8192, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "command task creation failed");
        }
    } else {
        ESP_LOGE(TAG, "command queue allocation failed");
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "input queue allocation failed");
        return;
    }
    if (xTaskCreate(input_task, "app_input", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "input task creation failed");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    s_input_ready = true;

    if (s_config.wifi_ssid[0]) {
        esp_err_t wifi_err = wifi_sta_start(s_config.wifi_ssid, s_config.wifi_pass);
        if (wifi_err == ESP_OK) {
            if (xTaskCreate(net_task, "app_net", 8192, NULL, 4, NULL) != pdPASS) {
                ESP_LOGE(TAG, "network task creation failed");
                ui_message("网络任务失败", "重启重试");
            }
        } else {
            ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(wifi_err));
            ui_message("WiFi 启动失败", "重新配网");
        }
    } else {
        ESP_LOGW(TAG, "WiFi credentials missing");
        ui_message("WiFi 未配置", "重新配网");
    }

    esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(button_err));
    } else if (s_key_guard.armed) {
        // If the wake key was released before the driver started observing it,
        // no release event will ever arrive; sync from the physical level.
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }

    s_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "usb host: %s", usb_serial_jtag_is_connected() ? "connected" : "absent");
    if (xTaskCreate(power_task, "app_power", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "power task creation failed");
    }

    ESP_LOGI(TAG, "ready: buttons=%s battery=%s",
             button_err == ESP_OK ? "ok" : "fail",
             battery_err == ESP_OK ? "ok" : "fail");
}
