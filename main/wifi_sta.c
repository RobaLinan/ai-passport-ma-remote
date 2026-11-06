#include "wifi_sta.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wifi_sta";

#define RETRY_FAST_MS   2000
#define RETRY_SLOW_MS   10000
#define RETRIES_BEFORE_SLOW 10

static volatile wifi_sta_state_t s_state = WIFI_STA_IDLE;
static esp_timer_handle_t s_retry_timer;
static int s_retries;
static bool s_started;

static void retry_cb(void *arg) {
    (void)arg;
    if (s_state == WIFI_STA_CONNECTED) return;
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(e));
    }
}

static void schedule_retry(void) {
    s_retries++;
    s_state = s_retries >= RETRIES_BEFORE_SLOW ? WIFI_STA_FAILED : WIFI_STA_CONNECTING;
    const uint32_t delay_ms = s_retries >= RETRIES_BEFORE_SLOW ? RETRY_SLOW_MS : RETRY_FAST_MS;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        s_state = WIFI_STA_CONNECTING;
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "associated with AP");
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected reason=%d retry=%d", ev ? ev->reason : -1, s_retries + 1);
        schedule_retry();
        break;
    }
    default:
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
    s_retries = 0;
    s_state = WIFI_STA_CONNECTED;
}

esp_err_t wifi_sta_start(const char *ssid, const char *pass) {
    if (!ssid || !pass || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_OK;

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) return ESP_FAIL;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "Passport-%02X%02X", mac[4], mac[5]);
    e = esp_netif_set_hostname(sta, hostname);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "set hostname failed: %s", esp_err_to_name(e));
    }

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&init);
    if (e != ESP_OK) return e;

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    e = esp_timer_create(&timer_args, &s_retry_timer);
    if (e != ESP_OK) return e;

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(e));
        return e;
    }

    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) ESP_LOGW(TAG, "disable power save failed: %s", esp_err_to_name(ps));

    // 2.4 GHz 40 MHz channels overlap heavily; force 20 MHz for stability.
    esp_err_t bw = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (bw != ESP_OK) ESP_LOGW(TAG, "set HT20 failed: %s", esp_err_to_name(bw));

    s_started = true;
    s_state = WIFI_STA_CONNECTING;
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    return ESP_OK;
}

wifi_sta_state_t wifi_sta_state(void) {
    return s_state;
}

void wifi_sta_set_power_save(bool enabled) {
    if (!s_started) return;
    const esp_err_t err = esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set power save %d failed: %s", enabled, esp_err_to_name(err));
    }
}
