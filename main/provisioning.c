#include "provisioning.h"

#include "app_ui.h"
#include "bsp_display.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "provision";

#define BODY_MAX 2048

static app_config_t *s_cfg;

static const char FORM_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AI Passport 配网</title><style>"
    "body{font-family:sans-serif;background:#0b0f14;color:#f2f5f8;margin:0;padding:24px}"
    "h2{color:#4cc2ff;margin:0 0 6px}p{color:#8a97a6;font-size:14px;line-height:1.5}"
    "label{display:block;margin:14px 0 6px;font-size:14px;color:#8a97a6}"
    "input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
    "border:1px solid #2a3441;background:#151c26;color:#f2f5f8;font-size:16px}"
    "button{width:100%;margin-top:22px;padding:12px;border:0;border-radius:8px;"
    "background:#4cc2ff;color:#06121b;font-size:16px;font-weight:600}"
    ".row{display:flex;gap:10px}.row>div{flex:1}</style></head><body>"
    "<h2>AI Passport 配网</h2>"
    "<p>连接此热点后填写家庭 WiFi 和服务器地址。<br>"
    "仅支持 2.4GHz WiFi；保存后设备会自动重启。</p>"
    "<form method='post' action='/save'>"
    "<label>WiFi 名称 (SSID)</label>"
    "<input name='ssid' required maxlength='32' autocomplete='off'>"
    "<label>WiFi 密码</label>"
    "<input name='pass' type='password' maxlength='64' autocomplete='off'>"
    "<label>MA 地址</label>"
    "<input name='ma_host' required maxlength='63' placeholder='192.168.1.10'>"
    "<div class='row'>"
    "<div><label>MA 端口</label><input name='ma_port' inputmode='numeric' value='8095'></div>"
    "<div><label>Bridge 端口</label><input name='bridge_port' inputmode='numeric' value='8096'></div>"
    "</div>"
    "<label>MA Token</label>"
    "<input name='ma_token' type='password' maxlength='511' autocomplete='off'>"
    "<label>Bridge 地址</label>"
    "<input name='bridge_host' required maxlength='63' placeholder='192.168.1.20'>"
    "<button type='submit'>保存并重启</button></form>"
    "<p style='margin-top:18px'>MA token 会写入设备 NVS；固件内置了 token 时可留空。</p>"
    "</body></html>";

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>已保存</title><style>body{font-family:sans-serif;background:#0b0f14;"
    "color:#f2f5f8;padding:32px}h2{color:#63d08b}</style></head><body>"
    "<h2>保存成功</h2><p>设备将在 1 秒后重启并连接 WiFi。</p></body></html>";

static void url_decode(char *dst, size_t cap, const char *src, size_t len) {
    size_t out = 0;
    for (size_t i = 0; i < len && out + 1 < cap; i++) {
        char c = src[i];
        if (c == '+') {
            dst[out++] = ' ';
        } else if (c == '%' && i + 2 < len && isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[out++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t cap) {
    const size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);
        if (eq && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            url_decode(out, cap, eq + 1, pair_len - (size_t)(eq + 1 - p));
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static void trim(char *text) {
    char *start = text;
    while (*start == ' ' || *start == '\t') start++;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }
}

static int form_port(const char *body, const char *key, int fallback) {
    char buf[8];
    if (!form_get(body, key, buf, sizeof(buf)) || !buf[0]) return fallback;
    char *end = NULL;
    const long value = strtol(buf, &end, 10);
    if (!end || *end != '\0' || value < 1 || value > 65535) return -1;
    return (int)value;
}

static void reboot_cb(void *arg) {
    (void)arg;
    esp_restart();
}

static esp_err_t send_error(httpd_req_t *req, const char *message) {
    char page[256];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
             "<body style='font-family:sans-serif;background:#0b0f14;color:#f2f5f8;padding:32px'>"
             "<h2 style='color:#e86a6a'>保存失败</h2><p>%s</p></body></html>",
             message);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req) {
    char body[BODY_MAX];
    const int content_len = req->content_len;
    if (content_len <= 0 || content_len >= (int)sizeof(body)) {
        return send_error(req, "请求内容过长或为空");
    }
    int received = 0;
    while (received < content_len) {
        const int r = httpd_req_recv(req, body + received, content_len - received);
        if (r <= 0) return send_error(req, "读取请求失败");
        received += r;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    char ma_host[64] = {0};
    char ma_token[512] = {0};
    char bridge_host[64] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return send_error(req, "WiFi 名称不能为空");
    }
    (void)form_get(body, "pass", pass, sizeof(pass));
    if (!form_get(body, "ma_host", ma_host, sizeof(ma_host)) || !ma_host[0]) {
        return send_error(req, "MA 地址不能为空");
    }
    (void)form_get(body, "ma_token", ma_token, sizeof(ma_token));
    if (ma_token[0]) {
        snprintf(s_cfg->ma_token, sizeof(s_cfg->ma_token), "%s", ma_token);
    }
    if (!s_cfg->ma_token[0]) {
        return send_error(req, "MA token 不能为空");
    }
    if (!form_get(body, "bridge_host", bridge_host, sizeof(bridge_host)) ||
        !bridge_host[0]) {
        return send_error(req, "Bridge 地址不能为空");
    }
    trim(ssid);
    trim(ma_host);
    trim(bridge_host);
    if (!ssid[0] || !ma_host[0] || !bridge_host[0]) {
        return send_error(req, "地址不能只包含空白");
    }
    const int ma_port = form_port(body, "ma_port", 8095);
    const int bridge_port = form_port(body, "bridge_port", 8096);
    if (ma_port < 0 || bridge_port < 0) {
        return send_error(req, "端口必须为 1-65535");
    }

    snprintf(s_cfg->wifi_ssid, sizeof(s_cfg->wifi_ssid), "%s", ssid);
    snprintf(s_cfg->wifi_pass, sizeof(s_cfg->wifi_pass), "%s", pass);
    snprintf(s_cfg->ma_host, sizeof(s_cfg->ma_host), "%s", ma_host);
    s_cfg->ma_port = (uint16_t)ma_port;
    snprintf(s_cfg->bridge_host, sizeof(s_cfg->bridge_host), "%s", bridge_host);
    s_cfg->bridge_port = (uint16_t)bridge_port;
    s_cfg->provisioned = true;

    const esp_err_t err = app_config_save_all(s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save config failed: %s", esp_err_to_name(err));
        return send_error(req, "写入 NVS 失败，请重试");
    }
    ESP_LOGI(TAG, "config saved: WiFi '%s', MA %s:%d, bridge %s:%d", ssid, ma_host,
             ma_port, bridge_host, bridge_port);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const esp_err_t send_err = httpd_resp_sendstr(req, SAVED_HTML);
    static esp_timer_handle_t reboot_timer;
    if (!reboot_timer) {
        const esp_timer_create_args_t args = {
            .callback = reboot_cb,
            .name = "prov_reboot",
        };
        if (esp_timer_create(&args, &reboot_timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_stop(reboot_timer);
    esp_timer_start_once(reboot_timer, 1500 * 1000);
    return send_err;
}

static void start_softap(const char *ssid) {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap() ? ESP_OK : ESP_FAIL);
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t wc = {0};
    snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "%s", ssid);
    wc.ap.ssid_len = strlen(ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP '%s' started, open http://192.168.4.1", ssid);
}

static void start_http(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_LOGI(TAG, "setup web server ready");
}

void provisioning_run(app_config_t *cfg) {
    s_cfg = cfg;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "Passport-%02X%02X", mac[4], mac[5]);

    if (bsp_lvgl_lock(1000)) {
        app_ui_set_provisioning(ssid);
        bsp_lvgl_unlock();
    }
    ESP_LOGW(TAG, "no usable configuration; starting setup AP");
    start_softap(ssid);
    start_http();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
