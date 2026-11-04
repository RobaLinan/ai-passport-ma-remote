#include "ota_server.h"

#include "app_ui.h"
#include "bsp_display.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ota";

#define OTA_BUF_SIZE 4096
#define OTA_REBOOT_DELAY_US (1500 * 1000)

static httpd_handle_t s_server;
static bool s_started;
static volatile bool s_busy;

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
    "</script></body></html>";

static void ui_progress(int percent) {
    if (bsp_lvgl_lock(100)) {
        app_ui_set_ota_progress(percent);
        bsp_lvgl_unlock();
    }
}

static void ui_result(const char *headline, const char *detail) {
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
    config.max_uri_handlers = 4;
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
    (void)httpd_register_uri_handler(s_server, &page);
    (void)httpd_register_uri_handler(s_server, &page_ota);
    (void)httpd_register_uri_handler(s_server, &upload);
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
