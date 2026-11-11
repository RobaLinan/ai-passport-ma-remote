#include "ota_server.h"

#include "app_config.h"
#include "app_ui.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota";

#define OTA_BUF_SIZE 4096
#define CONFIG_BODY_MAX 4096
#define OTA_REBOOT_DELAY_US (1500 * 1000)

static httpd_handle_t s_server;
static bool s_started;
static volatile bool s_busy;
static int64_t s_ui_hold_until_us;

static const char OTA_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AI Passport OTA</title><style>"
    "body{font-family:sans-serif;background:#0b0f14;color:#f2f5f8;margin:0;padding:24px;max-width:640px}"
    "h2{color:#4cc2ff;margin:0 0 8px}p,li{color:#8a97a6;font-size:14px;line-height:1.6}"
    "code{color:#f2f5f8}"
    "input{width:100%;box-sizing:border-box;margin:16px 0;padding:10px;background:#151c26;"
    "color:#f2f5f8;border:1px solid #2a3441;border-radius:8px}"
    "button{width:100%;padding:12px;border:0;border-radius:8px;background:#4cc2ff;"
    "color:#06121b;font-size:16px;font-weight:600}"
    "#s{margin-top:14px;color:#63d08b;font-size:14px;word-break:break-all}"
    "</style></head><body>"
    "<h2>AI Passport 固件升级</h2>"
    "<p>选择 <code>idf.py build</code> 生成的 app 镜像："
    "<code>build/FoloToy-AI-Passport.bin</code>。不要上传 "
    "<code>merged-binary.bin</code>。升级过程中保持供电，不要断电。</p>"
    "<input id='f' type='file' accept='.bin'>"
    "<button onclick='up()'>上传并升级</button><div id='s'></div>"
    "<script>"
    "async function up(){const f=document.getElementById('f').files[0];"
    "const s=document.getElementById('s');"
    "if(!f){s.textContent='请选择 .bin 文件';return;}"
    "if(!confirm('上传 '+f.name+' 并重启设备？'))return;"
    "s.textContent='上传中 0%';const xhr=new XMLHttpRequest();"
    "xhr.open('POST','/ota');"
    "xhr.setRequestHeader('Content-Type','application/octet-stream');"
    "xhr.upload.onprogress=e=>{if(e.lengthComputable)"
    "s.textContent='上传中 '+Math.round(e.loaded*100/e.total)+'%';};"
    "xhr.onload=()=>{s.textContent=xhr.responseText;};"
    "xhr.onerror=()=>{s.textContent='上传失败，请重试';};"
    "xhr.send(f);}"
    "</script>"
    "<p><a style='color:#4cc2ff' href='/config'>网络与服务器配置</a></p>"
    "</body></html>";

static void ui_progress(int percent) {
    if (bsp_lvgl_lock(100)) {
        app_ui_set_ota_progress(percent);
        bsp_lvgl_unlock();
    }
}

static void ui_result(const char *headline, const char *detail) {
    s_ui_hold_until_us = esp_timer_get_time() + 3LL * 1000 * 1000;
    if (bsp_lvgl_lock(100)) {
        app_ui_set_message(headline, detail);
        bsp_lvgl_unlock();
    }
}

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

static void reboot_cb(void *arg) {
    (void)arg;
    esp_restart();
}

static void schedule_reboot(void) {
    static esp_timer_handle_t timer;
    if (!timer) {
        const esp_timer_create_args_t args = {
            .callback = reboot_cb,
            .name = "ota_reboot",
        };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, OTA_REBOOT_DELAY_US);
}

static esp_err_t page_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, OTA_HTML, HTTPD_RESP_USE_STRLEN);
}

static void url_decode(char *dst, size_t cap, const char *src, size_t len) {
    size_t out = 0;
    for (size_t i = 0; i < len && out + 1 < cap; i++) {
        const char c = src[i];
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

static void html_escape(char *dst, size_t cap, const char *src) {
    size_t out = 0;
    for (; *src && out + 1 < cap; src++) {
        const char *rep = NULL;
        if (*src == '&') rep = "&amp;";
        else if (*src == '<') rep = "&lt;";
        else if (*src == '>') rep = "&gt;";
        else if (*src == '"') rep = "&quot;";
        if (rep) {
            const size_t n = strlen(rep);
            if (out + n >= cap) break;
            memcpy(dst + out, rep, n);
            out += n;
        } else {
            dst[out++] = *src;
        }
    }
    dst[out] = '\0';
}

static const char CONFIG_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AI Passport 配置</title><style>"
    "body{font-family:sans-serif;background:#0b0f14;color:#f2f5f8;margin:0;padding:24px;max-width:640px}"
    "h2{color:#4cc2ff;margin:0 0 8px}p{color:#8a97a6;font-size:14px;line-height:1.6}"
    "label{display:block;margin:14px 0 6px;font-size:14px;color:#8a97a6}"
    "input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a3441;"
    "background:#151c26;color:#f2f5f8;font-size:16px}"
    "button{width:100%;margin-top:22px;padding:12px;border:0;border-radius:8px;background:#4cc2ff;"
    "color:#06121b;font-size:16px;font-weight:600}"
    ".row{display:flex;gap:10px}.row>div{flex:1}"
    "</style></head><body><h2>网络与服务器配置</h2>"
    "<p>修改 WiFi、Music Assistant 或 bridge。密码和 token 留空表示保持当前值；"
    "保存后重启设备才生效。仅限可信局域网。</p><form method='post' action='/config'>"
    "<label>WiFi 名称 (SSID)</label>";

static const char CONFIG_PASS[] =
    "<label>WiFi 密码（留空保持不变）</label>"
    "<input name='pass' type='password' maxlength='64' autocomplete='off'>"
    "<label>MA 地址</label>";

static const char CONFIG_PORTS[] =
    "<div class='row'><div><label>MA 端口</label>"
    "<input name='ma_port' inputmode='numeric' value='%u'></div>"
    "<div><label>Bridge 端口</label>"
    "<input name='bridge_port' inputmode='numeric' value='%u'></div></div>"
    "<label>MA Token（留空保持不变）</label>"
    "<input name='ma_token' type='password' maxlength='511' autocomplete='off'>"
    "<label>Bridge 地址</label>";

static const char CONFIG_OPACITY[] =
    "<label>卡片不透明度 (%%)</label>"
    "<input name='card_opacity' type='number' min='0' max='100' value='%u'>"
    "<p>0 = 全透明，100 = 不透明；保存后立即生效。</p>";

static const char CONFIG_BATTERY[] =
    "<h2 style='margin-top:28px;font-size:18px'>电池校准</h2>"
    "<p>电量计：%s；电压：%s。校准后显示：%s。</p>"
    "<label>显示偏移 (%%)，范围 -30 到 30</label>"
    "<input name='batt_offset' type='number' min='-30' max='30' value='%d'>"
    "<p>显示电量 = 电量计读数 + 偏移，限制在 0-100；保存后立即生效。</p>";

static const char CONFIG_TAIL[] =
    "<button type='submit'>保存配置</button></form>"
    "<p><a style='color:#4cc2ff' href='/'>返回升级页</a></p></body></html>";

static esp_err_t chunk_send(httpd_req_t *req, const char *text) {
    return httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get(httpd_req_t *req) {
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    (void)app_config_load(&cfg);

    char ssid[96], ma_host[160], bridge_host[160];
    html_escape(ssid, sizeof(ssid), cfg.wifi_ssid);
    html_escape(ma_host, sizeof(ma_host), cfg.ma_host);
    html_escape(bridge_host, sizeof(bridge_host), cfg.bridge_host);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (chunk_send(req, CONFIG_HEAD) != ESP_OK) return ESP_FAIL;
    char line[1024];
    snprintf(line, sizeof(line),
             "<input name='ssid' required maxlength='32' autocomplete='off' value='%s'>", ssid);
    if (chunk_send(req, line) != ESP_OK) return ESP_FAIL;
    if (chunk_send(req, CONFIG_PASS) != ESP_OK) return ESP_FAIL;
    snprintf(line, sizeof(line),
             "<input name='ma_host' required maxlength='63' value='%s'>", ma_host);
    if (chunk_send(req, line) != ESP_OK) return ESP_FAIL;
    snprintf(line, sizeof(line), CONFIG_PORTS, (unsigned)cfg.ma_port,
             (unsigned)cfg.bridge_port);
    if (chunk_send(req, line) != ESP_OK) return ESP_FAIL;
    snprintf(line, sizeof(line),
             "<input name='bridge_host' required maxlength='63' value='%s'>", bridge_host);
    if (chunk_send(req, line) != ESP_OK) return ESP_FAIL;
    snprintf(line, sizeof(line), CONFIG_OPACITY, (unsigned)cfg.card_opacity);
    if (chunk_send(req, line) != ESP_OK) return ESP_FAIL;

    const int raw_soc = bsp_battery_soc();
    const int mv = bsp_battery_mv();
    char soc_text[16], mv_text[16], cal_text[16];
    if (raw_soc < 0) snprintf(soc_text, sizeof(soc_text), "--");
    else snprintf(soc_text, sizeof(soc_text), "%d%%", raw_soc);
    if (mv < 0) snprintf(mv_text, sizeof(mv_text), "--");
    else snprintf(mv_text, sizeof(mv_text), "%d mV", mv);
    int cal_soc = raw_soc < 0 ? -1 : raw_soc + cfg.battery_offset;
    if (cal_soc < 0) cal_soc = 0;
    if (cal_soc > 100) cal_soc = 100;
    if (raw_soc < 0) snprintf(cal_text, sizeof(cal_text), "--");
    else snprintf(cal_text, sizeof(cal_text), "%d%%", cal_soc);
    snprintf(line, sizeof(line), CONFIG_BATTERY, soc_text, mv_text, cal_text,
             (int)cfg.battery_offset);
    if (chunk_send(req, line) != ESP_OK) return ESP_FAIL;
    if (chunk_send(req, CONFIG_TAIL) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t config_post(httpd_req_t *req) {
    if (s_busy) {
        return send_text(req, "409 Conflict", "升级进行中，稍后再改配置");
    }
    char body[CONFIG_BODY_MAX];
    const int content_len = req->content_len;
    if (content_len <= 0 || content_len >= (int)sizeof(body)) {
        return send_text(req, "400 Bad Request", "请求内容为空或过长");
    }
    int received = 0;
    while (received < content_len) {
        const int r = httpd_req_recv(req, body + received, content_len - received);
        if (r <= 0) return send_text(req, "400 Bad Request", "读取请求失败");
        received += r;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    char ma_host[64] = {0};
    char ma_token[512] = {0};
    char bridge_host[64] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return send_text(req, "400 Bad Request", "WiFi 名称不能为空");
    }
    (void)form_get(body, "pass", pass, sizeof(pass));
    (void)form_get(body, "ma_host", ma_host, sizeof(ma_host));
    (void)form_get(body, "ma_token", ma_token, sizeof(ma_token));
    (void)form_get(body, "bridge_host", bridge_host, sizeof(bridge_host));
    if (!ma_host[0]) return send_text(req, "400 Bad Request", "MA 地址不能为空");
    if (!bridge_host[0]) return send_text(req, "400 Bad Request", "Bridge 地址不能为空");
    char opacity_buf[8];
    int card_opacity = -1;
    if (form_get(body, "card_opacity", opacity_buf, sizeof(opacity_buf))) {
        char *end = NULL;
        const long value = strtol(opacity_buf, &end, 10);
        if (!end || *end != '\0' || value < 0 || value > 100) {
            return send_text(req, "400 Bad Request", "卡片不透明度必须为 0-100");
        }
        card_opacity = (int)value;
    }
    trim(ssid);
    trim(ma_host);
    trim(bridge_host);
    if (!ssid[0] || !ma_host[0] || !bridge_host[0]) {
        return send_text(req, "400 Bad Request", "地址不能只包含空白");
    }

    app_config_t cfg;
    if (app_config_load(&cfg) != ESP_OK) {
        return send_text(req, "500 Internal Server Error", "读取配置失败");
    }
    const int ma_port = form_port(body, "ma_port", cfg.ma_port);
    const int bridge_port = form_port(body, "bridge_port", cfg.bridge_port);
    if (ma_port < 0 || bridge_port < 0) {
        return send_text(req, "400 Bad Request", "端口必须为 1-65535");
    }
    char offset_buf[8];
    if (form_get(body, "batt_offset", offset_buf, sizeof(offset_buf))) {
        char *end = NULL;
        const long value = strtol(offset_buf, &end, 10);
        if (!end || *end != '\0' || value < -30 || value > 30) {
            return send_text(req, "400 Bad Request", "电量偏移必须为 -30 到 30");
        }
        cfg.battery_offset = (int8_t)value;
    }

    snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid);
    if (pass[0]) snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", pass);
    snprintf(cfg.ma_host, sizeof(cfg.ma_host), "%s", ma_host);
    cfg.ma_port = (uint16_t)ma_port;
    if (ma_token[0]) snprintf(cfg.ma_token, sizeof(cfg.ma_token), "%s", ma_token);
    snprintf(cfg.bridge_host, sizeof(cfg.bridge_host), "%s", bridge_host);
    cfg.bridge_port = (uint16_t)bridge_port;
    if (card_opacity >= 0) cfg.card_opacity = (uint8_t)card_opacity;
    cfg.provisioned = true;

    const esp_err_t err = app_config_save_all(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
        return send_text(req, "500 Internal Server Error", "写入 NVS 失败");
    }
    ESP_LOGI(TAG, "config saved: WiFi '%s', MA %s:%d, bridge %s:%d, opacity %u, "
                  "battery offset %d",
             cfg.wifi_ssid, cfg.ma_host, cfg.ma_port, cfg.bridge_host, cfg.bridge_port,
             (unsigned)cfg.card_opacity, (int)cfg.battery_offset);
    if (bsp_lvgl_lock(200)) {
        app_ui_set_card_opacity(cfg.card_opacity);
        app_ui_set_battery_offset(cfg.battery_offset);
        bsp_lvgl_unlock();
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(
        req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
        "<body style='font-family:sans-serif;background:#0b0f14;color:#f2f5f8;padding:32px'>"
        "<h2 style='color:#63d08b'>配置已保存</h2>"
        "<p>WiFi/MA 重启后生效；卡片不透明度和电量偏移已立即应用。</p>"
        "<p><a style='color:#4cc2ff' href='/config'>返回配置</a></p></body></html>");
}

static esp_err_t ota_post(httpd_req_t *req) {
    if (s_busy) {
        return send_text(req, "409 Conflict", "已有升级任务在进行");
    }
    const int total = req->content_len;
    if (total <= 0) {
        return send_text(req, "411 Length Required", "缺少 Content-Length");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_text(req, "500 Internal Server Error", "没有可用的 OTA 分区");
    }
    if ((size_t)total > part->size) {
        return send_text(req, "400 Bad Request", "镜像大于 OTA 分区，请确认上传的是 app bin");
    }

    ESP_LOGI(TAG, "OTA start: %d bytes -> %s at 0x%lx", total, part->label,
             (unsigned long)part->address);
    s_busy = true;
    ui_progress(0);

    esp_ota_handle_t handle = 0;
    bool handle_open = false;
    esp_err_t err = esp_ota_begin(part, total, &handle);
    if (err == ESP_OK) handle_open = true;

    char buf[OTA_BUF_SIZE];
    int remaining = total;
    int last_percent = -1;
    int timeouts = 0;
    while (err == ESP_OK && remaining > 0) {
        const int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        const int received = httpd_req_recv(req, buf, want);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(handle, buf, received);
        if (err != ESP_OK) break;
        remaining -= received;
        const int percent = (total - remaining) * 100 / total;
        if (percent != last_percent && percent % 2 == 0) {
            last_percent = percent;
            ui_progress(percent);
        }
    }

    if (err == ESP_OK && remaining == 0) {
        err = esp_ota_end(handle);
        handle_open = false;
    } else if (handle_open) {
        esp_ota_abort(handle);
        handle_open = false;
    }
    if (remaining != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        s_busy = false;
        ui_result("升级失败", "请重试");
        return send_text(req, "500 Internal Server Error", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "OTA success: %d bytes, boot partition set", total);
    ui_progress(100);
    ui_result("升级完成", "设备重启中");
    const esp_err_t send_err = send_text(req, "200 OK", "升级成功，设备正在重启…");
    schedule_reboot();
    return send_err;
}

esp_err_t ota_server_start(void) {
    if (s_started) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 6;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA server start failed: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t page = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = page_get,
    };
    const httpd_uri_t page_ota = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = page_get,
    };
    const httpd_uri_t upload = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post,
    };
    const httpd_uri_t config_page = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get,
    };
    const httpd_uri_t config_save = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = config_post,
    };
    (void)httpd_register_uri_handler(s_server, &page);
    (void)httpd_register_uri_handler(s_server, &page_ota);
    (void)httpd_register_uri_handler(s_server, &upload);
    (void)httpd_register_uri_handler(s_server, &config_page);
    (void)httpd_register_uri_handler(s_server, &config_save);
    s_started = true;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            ESP_LOGI(TAG, "OTA page: http://" IPSTR "/", IP2STR(&ip.ip));
        }
    }
    return ESP_OK;
}

bool ota_server_busy(void) {
    return s_busy;
}

bool ota_server_ui_blocked(void) {
    return s_busy || esp_timer_get_time() < s_ui_hold_until_us;
}
