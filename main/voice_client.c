#include "voice_client.h"

#include "app_ui.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "voice";

#define VOICE_SAMPLE_RATE 16000
#define VOICE_MAX_SECONDS 5
#define VOICE_TOTAL_BYTES (VOICE_SAMPLE_RATE * 2 * VOICE_MAX_SECONDS)
// 0.9 s at 16 kHz/16-bit. Covers the 800 ms long-press threshold plus input
// dispatch latency, so a short OK press is cancelled before any upload starts.
#define VOICE_PREBUF_BYTES 28672
#define VOICE_CHUNK_BYTES 2048
#define VOICE_EVENT_QUEUE_DEPTH 4

typedef enum {
    VOICE_EV_START = 0,
    VOICE_EV_LISTENING,
    VOICE_EV_COMMIT,
    VOICE_EV_STOP,
    VOICE_EV_CANCEL,
} voice_event_type_t;

typedef struct {
    voice_event_type_t type;
    uint32_t seq;
    char player_id[96];
    char queue_id[96];
} voice_event_t;

static app_config_t s_cfg;
static QueueHandle_t s_events;
static bool s_audio_ok;
static volatile bool s_busy;
static volatile int64_t s_ui_hold_until_us;
static uint32_t s_generation;

static void ui_msg(const char *headline, const char *detail) {
    if (bsp_lvgl_lock(300)) {
        app_ui_set_message(headline, detail);
        bsp_lvgl_unlock();
    }
}

static void ui_hold(int ms) {
    s_ui_hold_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static void send_event(voice_event_type_t type, const char *player_id, const char *queue_id) {
    if (!s_events) return;
    voice_event_t ev = { .type = type, .seq = s_generation };
    if (player_id) snprintf(ev.player_id, sizeof(ev.player_id), "%s", player_id);
    if (queue_id) snprintf(ev.queue_id, sizeof(ev.queue_id), "%s", queue_id);
    (void)xQueueSend(s_events, &ev, 0);
}

static void show_listening(void) {
    if (bsp_lvgl_lock(300)) {
        app_ui_set_message("正在听", "松开结束");
        app_ui_set_listening(true);
        bsp_lvgl_unlock();
    }
}

static void hide_wave(void) {
    if (bsp_lvgl_lock(100)) {
        app_ui_set_listening(false);
        bsp_lvgl_unlock();
    }
}

static int pcm_peak(const uint8_t *buf, size_t len) {
    const int16_t *samples = (const int16_t *)buf;
    int peak = 0;
    for (size_t i = 0; i < len / 2; i++) {
        int v = samples[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return peak;
}

static int pcm_level(const uint8_t *buf, size_t len) {
    const int peak = pcm_peak(buf, len);
    if (peak < 180) return 0;
    const int level = peak / 128;
    return level > 100 ? 100 : level;
}

static void update_wave(const uint8_t *buf, size_t len, int64_t *last_us) {
    const int64_t now = esp_timer_get_time();
    if (now - *last_us < 80000) return;
    *last_us = now;
    const int level = pcm_level(buf, len);
    if (bsp_lvgl_lock(30)) {
        app_ui_set_audio_level(level);
        bsp_lvgl_unlock();
    }
}

static void consume_events(uint32_t seq, bool *listening, bool *done, bool *cancel) {
    voice_event_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.seq != seq) continue;
        if (ev.type == VOICE_EV_LISTENING) {
            if (!*listening) {
                *listening = true;
                show_listening();
            }
        } else if (ev.type == VOICE_EV_COMMIT || ev.type == VOICE_EV_STOP) {
            *done = true;
        } else if (ev.type == VOICE_EV_CANCEL) {
            *cancel = true;
        }
    }
}

static bool audio_begin(void) {
    if (!s_audio_ok) return false;
    if (bsp_audio_wake() != ESP_OK) return false;
    return bsp_audio_set_format(VOICE_SAMPLE_RATE, 16, 1) == ESP_OK;
}

static void audio_end(void) {
    if (!s_audio_ok) return;
    esp_err_t err = bsp_audio_sleep();
    if (err != ESP_OK) ESP_LOGW(TAG, "audio sleep failed: %s", esp_err_to_name(err));
}

static void report_result(bool ok, const char *display, const char *transcript,
                          const char *error) {
    if (ok) {
        ui_msg(display && display[0] ? display : "已发送",
               transcript && transcript[0] ? transcript : "");
    } else if (!transcript || !transcript[0]) {
        ui_msg("没有听清", "请再说一次");
    } else if (error && strstr(error, "matched")) {
        ui_msg("没找到", transcript);
    } else if (error && (strstr(error, "bridge") || strstr(error, "HTTP"))) {
        ui_msg("服务不可用", "检查 bridge");
    } else {
        ui_msg("识别失败", transcript);
    }
    ui_hold(2500);
}

static void run_ptt(const voice_event_t *start, uint8_t *prebuf) {
    s_busy = true;

    const int64_t audio_t0 = esp_timer_get_time();
    const bool audio_started = audio_begin();
    ESP_LOGI(TAG, "audio_begin %lld ms ok=%d",
             (long long)((esp_timer_get_time() - audio_t0) / 1000), audio_started);
    if (!audio_started) {
        ESP_LOGE(TAG, "audio start failed");
        ui_msg("麦克风不可用", "检查音频");
        ui_hold(2500);
        s_busy = false;
        return;
    }

    bool listening = false;
    bool prebuf_full = false;
    bool done = false;
    bool cancel = false;
    size_t pre_len = 0;
    int64_t last_wave_us = 0;

    while (!done && !cancel) {
        consume_events(start->seq, &listening, &done, &cancel);
        if (done || cancel) break;

        if (!prebuf_full) {
            size_t room = VOICE_PREBUF_BYTES - pre_len;
            size_t chunk = room < VOICE_CHUNK_BYTES ? room : VOICE_CHUNK_BYTES;
            if (bsp_audio_read(prebuf + pre_len, chunk) != ESP_OK) {
                ESP_LOGE(TAG, "audio read failed");
                hide_wave();
                audio_end();
                ui_msg("录音失败", "请重试");
                ui_hold(2500);
                s_busy = false;
                return;
            }
            if (listening) update_wave(prebuf + pre_len, chunk, &last_wave_us);
            pre_len += chunk;
            if (pre_len >= VOICE_PREBUF_BYTES) prebuf_full = true;
        } else if (listening) {
            // Buffer is full and the long-press was recognized: start the
            // upload and keep streaming live audio while the key is held.
            break;
        } else {
            // Do not upload before the long-press event: a short press must
            // remain a play/pause click and never reach the bridge.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (cancel) {
        ESP_LOGI(TAG, "ptt cancelled: prebuf=%u", (unsigned)pre_len);
        hide_wave();
        audio_end();
        s_busy = false;
        return;
    }

    const bool live = !done;
    bool recognizing = false;
    if (done) {
        ui_msg("识别中", "请稍候");
        recognizing = true;
    }
    ESP_LOGI(TAG, "prebuf=%u bytes peak=%d live=%d",
             (unsigned)pre_len, pcm_peak(prebuf, pre_len), live);

    char url[300];
    if (start->queue_id[0]) {
        snprintf(url, sizeof(url), "http://%s:%u/voice?player_id=%s&queue_id=%s",
                 s_cfg.bridge_host, s_cfg.bridge_port, start->player_id,
                 start->queue_id);
    } else {
        snprintf(url, sizeof(url), "http://%s:%u/voice?player_id=%s",
                 s_cfg.bridge_host, s_cfg.bridge_port, start->player_id);
    }
    ESP_LOGI(TAG, "uploading %u prebuffered bytes to %s", (unsigned)pre_len,
             s_cfg.bridge_host);

    const esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    bool failed = client == NULL;
    bool opened = false;
    size_t sent = 0;

    if (!failed) {
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        if (esp_http_client_open(client, VOICE_TOTAL_BYTES) != ESP_OK) {
            failed = true;
        } else {
            opened = true;
        }
    }

    if (!failed && pre_len > 0) {
        if (esp_http_client_write(client, (const char *)prebuf, (int)pre_len) !=
            (int)pre_len) {
            failed = true;
        } else {
            sent = pre_len;
        }
    }

    consume_events(start->seq, &listening, &done, &cancel);
    if (cancel) {
        if (opened) esp_http_client_close(client);
        if (client) esp_http_client_cleanup(client);
        hide_wave();
        audio_end();
        s_busy = false;
        return;
    }

    static const uint8_t silence[VOICE_CHUNK_BYTES];
    uint8_t chunk[VOICE_CHUNK_BYTES];
    while (!failed && live && !done && !cancel && sent < VOICE_TOTAL_BYTES) {
        consume_events(start->seq, &listening, &done, &cancel);
        if (done || cancel) break;

        size_t want = VOICE_TOTAL_BYTES - sent;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        if (bsp_audio_read(chunk, want) != ESP_OK) {
            ESP_LOGE(TAG, "audio read failed while streaming");
            failed = true;
            break;
        }
        update_wave(chunk, want, &last_wave_us);
        if (esp_http_client_write(client, (const char *)chunk, (int)want) !=
            (int)want) {
            failed = true;
            break;
        }
        sent += want;
    }

    if (cancel) {
        if (opened) esp_http_client_close(client);
        if (client) esp_http_client_cleanup(client);
        hide_wave();
        audio_end();
        s_busy = false;
        return;
    }

    if (!recognizing) {
        ui_msg("识别中", "请稍候");
        recognizing = true;
    }

    while (!failed && sent < VOICE_TOTAL_BYTES) {
        size_t want = VOICE_TOTAL_BYTES - sent;
        if (want > sizeof(silence)) want = sizeof(silence);
        if (esp_http_client_write(client, (const char *)silence, (int)want) !=
            (int)want) {
            failed = true;
            break;
        }
        sent += want;
    }

    char response[2048] = {0};
    if (!failed && opened) {
        (void)esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        size_t total = 0;
        if (status == 200) {
            while (total + 1 < sizeof(response)) {
                const int r = esp_http_client_read(client, response + total,
                                                   (int)(sizeof(response) - 1 - total));
                if (r <= 0) break;
                total += (size_t)r;
            }
            response[total] = '\0';
        } else {
            ESP_LOGW(TAG, "bridge HTTP %d", status);
            failed = true;
        }
    }

    if (opened) esp_http_client_close(client);
    if (client) esp_http_client_cleanup(client);
    audio_end();
    ESP_LOGI(TAG, "upload sent=%u failed=%d response=%u bytes", (unsigned)sent,
             failed, (unsigned)strlen(response));

    if (failed) {
        report_result(false, NULL, NULL, "bridge HTTP");
    } else {
        cJSON *root = cJSON_Parse(response);
        if (!cJSON_IsObject(root)) {
            ESP_LOGW(TAG, "bridge response parse failed: %.120s", response);
            report_result(false, NULL, NULL, "HTTP parse");
        } else {
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
            const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
            const cJSON *transcript =
                cJSON_GetObjectItemCaseSensitive(root, "transcript");
            const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
            const bool success = cJSON_IsTrue(ok);
            ESP_LOGI(TAG, "bridge result ok=%d display='%s' transcript='%s'",
                     success,
                     cJSON_IsString(display) ? display->valuestring : "",
                     cJSON_IsString(transcript) ? transcript->valuestring : "");
            report_result(success,
                          cJSON_IsString(display) ? display->valuestring : NULL,
                          cJSON_IsString(transcript) ? transcript->valuestring : NULL,
                          cJSON_IsString(error) ? error->valuestring : NULL);
            cJSON_Delete(root);
        }
    }

    s_busy = false;
}

static void voice_task(void *arg) {
    (void)arg;
    for (;;) {
        voice_event_t ev;
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type != VOICE_EV_START) continue;
        uint8_t *prebuf = malloc(VOICE_PREBUF_BYTES);
        if (!prebuf) {
            ESP_LOGE(TAG, "prebuffer alloc failed (heap %u)", esp_get_free_heap_size());
            ui_msg("内存不足", "无法录音");
            ui_hold(2500);
            continue;
        }
        run_ptt(&ev, prebuf);
        free(prebuf);
    }
}

esp_err_t voice_client_init(const app_config_t *cfg) {
    s_cfg = *cfg;
    esp_err_t err = bsp_audio_init();
    s_audio_ok = err == ESP_OK;
    if (!s_audio_ok) {
        ESP_LOGW(TAG, "audio init failed: %s", esp_err_to_name(err));
    }
    s_events = xQueueCreate(VOICE_EVENT_QUEUE_DEPTH, sizeof(voice_event_t));
    if (!s_events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(voice_task, "app_voice", 8192, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "bridge %s:%u, audio=%s", s_cfg.bridge_host, s_cfg.bridge_port,
             s_audio_ok ? "ok" : "fail");
    return ESP_OK;
}

void voice_client_start(const char *player_id, const char *queue_id) {
    if (s_busy || !s_events) return;
    (void)xQueueReset(s_events);
    s_generation++;
    send_event(VOICE_EV_START, player_id, queue_id);
}

void voice_client_listening(void) {
    send_event(VOICE_EV_LISTENING, NULL, NULL);
}

void voice_client_commit(void) {
    send_event(VOICE_EV_COMMIT, NULL, NULL);
}

void voice_client_stop(void) {
    send_event(VOICE_EV_STOP, NULL, NULL);
}

void voice_client_cancel(void) {
    send_event(VOICE_EV_CANCEL, NULL, NULL);
}

bool voice_client_busy(void) {
    return s_busy;
}

bool voice_client_ui_blocked(void) {
    return s_busy || esp_timer_get_time() < s_ui_hold_until_us;
}
