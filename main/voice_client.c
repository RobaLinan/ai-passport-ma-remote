#include "voice_client.h"

#include "app_ui.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
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
#define VOICE_PREBUF_BYTES 20000
#define VOICE_CHUNK_BYTES 2048
#define VOICE_EVENT_QUEUE_DEPTH 4

typedef enum {
    VOICE_EV_START = 0,
    VOICE_EV_COMMIT,
    VOICE_EV_STOP,
    VOICE_EV_CANCEL,
} voice_event_type_t;

typedef struct {
    voice_event_type_t type;
    char player_id[96];
    char queue_id[96];
} voice_event_t;

static app_config_t s_cfg;
static QueueHandle_t s_events;
static bool s_audio_ok;
static volatile bool s_busy;
static volatile int64_t s_ui_hold_until_us;

static void ui_msg(const char *headline, const char *detail) {
    if (bsp_lvgl_lock(300)) {
        app_ui_set_message(headline, detail);
        bsp_lvgl_unlock();
    }
}

static void ui_hold(int ms) {
    s_ui_hold_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static void drain_events(void) {
    voice_event_t ev;
    if (!s_events) return;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
    }
}

static void send_event(voice_event_type_t type, const char *player_id, const char *queue_id) {
    if (!s_events) return;
    voice_event_t ev = { .type = type };
    if (player_id) snprintf(ev.player_id, sizeof(ev.player_id), "%s", player_id);
    if (queue_id) snprintf(ev.queue_id, sizeof(ev.queue_id), "%s", queue_id);
    (void)xQueueSend(s_events, &ev, 0);
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
    ui_msg("正在听", "松开结束");

    const int64_t audio_t0 = esp_timer_get_time();
    const bool audio_started = audio_begin();
    ESP_LOGI(TAG, "audio_begin %lld ms ok=%d",
             (long long)((esp_timer_get_time() - audio_t0) / 1000), audio_started);
    if (!audio_started) {
        ESP_LOGE(TAG, "audio start failed");
        ui_msg("麦克风不可用", "检查音频");
        ui_hold(2500);
        s_busy = false;
        drain_events();
        return;
    }

    bool committed = false;
    bool stop = false;
    bool cancel = false;
    size_t pre_len = 0;

    while (!committed && !stop && !cancel) {
        voice_event_t ev;
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            if (ev.type == VOICE_EV_COMMIT) committed = true;
            else if (ev.type == VOICE_EV_STOP) stop = true;
            else if (ev.type == VOICE_EV_CANCEL) cancel = true;
        }
        if (committed || stop || cancel) break;

        size_t room = VOICE_PREBUF_BYTES - pre_len;
        size_t chunk = room < VOICE_CHUNK_BYTES ? room : VOICE_CHUNK_BYTES;
        if (chunk == 0) {
            committed = true;
            break;
        }
        if (bsp_audio_read(prebuf + pre_len, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "audio read failed");
            audio_end();
            ui_msg("录音失败", "请重试");
            ui_hold(2500);
            s_busy = false;
            drain_events();
            return;
        }
        pre_len += chunk;
        if (pre_len >= VOICE_PREBUF_BYTES) committed = true;
    }

    if (cancel || (stop && !committed)) {
        ESP_LOGI(TAG, "ptt cancelled: prebuf=%u committed=%d stop=%d",
                 (unsigned)pre_len, committed, stop);
        audio_end();
        s_busy = false;
        drain_events();
        return;
    }

    int peak = 0;
    const int16_t *samples = (const int16_t *)prebuf;
    for (size_t i = 0; i < pre_len / 2; i++) {
        int v = samples[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    ESP_LOGI(TAG, "prebuf=%u bytes peak=%d committed=%d stop=%d",
             (unsigned)pre_len, peak, committed, stop);

    ui_msg("识别中", "请稍候");

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

    static const uint8_t silence[VOICE_CHUNK_BYTES];
    uint8_t chunk[VOICE_CHUNK_BYTES];
    while (!failed && !stop && !cancel && sent < VOICE_TOTAL_BYTES) {
        voice_event_t ev;
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            if (ev.type == VOICE_EV_STOP) stop = true;
            else if (ev.type == VOICE_EV_CANCEL) cancel = true;
        }
        if (stop || cancel) break;

        size_t want = VOICE_TOTAL_BYTES - sent;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        if (bsp_audio_read(chunk, want) != ESP_OK) {
            ESP_LOGE(TAG, "audio read failed while streaming");
            failed = true;
            break;
        }
        if (esp_http_client_write(client, (const char *)chunk, (int)want) !=
            (int)want) {
            failed = true;
            break;
        }
        sent += want;
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
    drain_events();
}

static void voice_task(void *arg) {
    (void)arg;
    uint8_t *prebuf = malloc(VOICE_PREBUF_BYTES);
    if (!prebuf) {
        ESP_LOGE(TAG, "voice prebuffer allocation failed");
        s_audio_ok = false;
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        voice_event_t ev;
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type != VOICE_EV_START) continue;
        run_ptt(&ev, prebuf);
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
    if (s_busy) return;
    send_event(VOICE_EV_START, player_id, queue_id);
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
