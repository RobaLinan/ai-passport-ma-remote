#include "app_config.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  else
#    pragma message("main/secrets.h not found: building with empty WiFi/MA defaults")
#    define APP_SECRET_WIFI_SSID ""
#    define APP_SECRET_WIFI_PASS ""
#    define APP_SECRET_MA_HOST   ""
#    define APP_SECRET_MA_PORT   8095
#    define APP_SECRET_MA_TOKEN  ""
#    define APP_SECRET_PLAYER_ID ""
#  endif
#else
#  include "secrets.h"
#endif

// Normal use provisions these through the setup AP; keep build-time defaults empty.
#ifndef APP_SECRET_BRIDGE_HOST
#define APP_SECRET_BRIDGE_HOST ""
#endif
#ifndef APP_SECRET_BRIDGE_PORT
#define APP_SECRET_BRIDGE_PORT 8096
#endif

#define NVS_NS "appcfg"

static const char *TAG = "app_cfg";

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

static void set_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    copy_str(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), APP_SECRET_WIFI_SSID);
    copy_str(cfg->wifi_pass, sizeof(cfg->wifi_pass), APP_SECRET_WIFI_PASS);
    copy_str(cfg->ma_host, sizeof(cfg->ma_host), APP_SECRET_MA_HOST);
    copy_str(cfg->ma_token, sizeof(cfg->ma_token), APP_SECRET_MA_TOKEN);
    copy_str(cfg->player_id, sizeof(cfg->player_id), APP_SECRET_PLAYER_ID);
    copy_str(cfg->bridge_host, sizeof(cfg->bridge_host), APP_SECRET_BRIDGE_HOST);
    cfg->ma_port = APP_SECRET_MA_PORT;
    cfg->bridge_port = APP_SECRET_BRIDGE_PORT;
    cfg->brightness = 100;
    cfg->card_opacity = 75;
    cfg->sleep_timeout_s = 60;
    cfg->provisioned = false;
}

static bool read_str(nvs_handle_t nvs, const char *key, char *dst, size_t cap) {
    size_t len = cap;
    esp_err_t e = nvs_get_str(nvs, key, dst, &len);
    if (e == ESP_OK) return true;
    if (e != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read '%s' failed: %s", key, esp_err_to_name(e));
    }
    return false;
}

esp_err_t app_config_load(app_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    set_defaults(cfg);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(e));
        if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK) {
            return ESP_FAIL;
        }
        e = ESP_OK;
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(e));
        return e;
    }

    nvs_handle_t nvs;
    e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(e));
        return e;
    }

    bool seed = false;
    char probe[64];
    size_t probe_len = sizeof(probe);
    const bool had_wifi = nvs_get_str(nvs, "wifi_ssid", probe, &probe_len) == ESP_OK;
    uint8_t provisioned;
    if (nvs_get_u8(nvs, "provisioned", &provisioned) == ESP_OK) {
        cfg->provisioned = provisioned != 0;
    } else {
        // Preserve an existing installation when upgrading; a blank NVS enters setup.
        cfg->provisioned = had_wifi;
        seed = true;
    }
    seed |= !read_str(nvs, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    seed |= !read_str(nvs, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    seed |= !read_str(nvs, "ma_host", cfg->ma_host, sizeof(cfg->ma_host));
    seed |= !read_str(nvs, "ma_token", cfg->ma_token, sizeof(cfg->ma_token));
    seed |= !read_str(nvs, "player_id", cfg->player_id, sizeof(cfg->player_id));
    seed |= !read_str(nvs, "bridge_host", cfg->bridge_host, sizeof(cfg->bridge_host));

    uint16_t port;
    if (nvs_get_u16(nvs, "ma_port", &port) == ESP_OK) cfg->ma_port = port;
    else seed = true;
    uint16_t bridge_port;
    if (nvs_get_u16(nvs, "bridge_port", &bridge_port) == ESP_OK) cfg->bridge_port = bridge_port;
    else seed = true;
    uint8_t brightness;
    if (nvs_get_u8(nvs, "brightness", &brightness) == ESP_OK) cfg->brightness = brightness;
    else seed = true;
    uint8_t card_opacity;
    if (nvs_get_u8(nvs, "card_opacity", &card_opacity) == ESP_OK &&
        card_opacity <= 100) {
        cfg->card_opacity = card_opacity;
    }
    uint16_t sleep_s;
    if (nvs_get_u16(nvs, "sleep_timeout_s", &sleep_s) == ESP_OK) cfg->sleep_timeout_s = sleep_s;
    else seed = true;

    if (seed) {
        nvs_set_str(nvs, "wifi_ssid", cfg->wifi_ssid);
        nvs_set_str(nvs, "wifi_pass", cfg->wifi_pass);
        nvs_set_str(nvs, "ma_host", cfg->ma_host);
        nvs_set_str(nvs, "ma_token", cfg->ma_token);
        nvs_set_str(nvs, "player_id", cfg->player_id);
        nvs_set_str(nvs, "bridge_host", cfg->bridge_host);
        nvs_set_u16(nvs, "ma_port", cfg->ma_port);
        nvs_set_u16(nvs, "bridge_port", cfg->bridge_port);
        nvs_set_u8(nvs, "brightness", cfg->brightness);
        nvs_set_u16(nvs, "sleep_timeout_s", cfg->sleep_timeout_s);
        nvs_set_u8(nvs, "provisioned", cfg->provisioned ? 1 : 0);
        e = nvs_commit(nvs);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "seeded NVS config: WiFi '%s', MA %s:%u",
                     cfg->wifi_ssid, cfg->ma_host, cfg->ma_port);
        } else {
            ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(e));
        }
    }

    nvs_close(nvs);
    return e;
}

esp_err_t app_config_save_all(const app_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_str(nvs, "wifi_ssid", cfg->wifi_ssid);
    if (e == ESP_OK) e = nvs_set_str(nvs, "wifi_pass", cfg->wifi_pass);
    if (e == ESP_OK) e = nvs_set_str(nvs, "ma_host", cfg->ma_host);
    if (e == ESP_OK) e = nvs_set_u16(nvs, "ma_port", cfg->ma_port);
    if (e == ESP_OK) e = nvs_set_str(nvs, "ma_token", cfg->ma_token);
    if (e == ESP_OK) e = nvs_set_str(nvs, "player_id", cfg->player_id);
    if (e == ESP_OK) e = nvs_set_str(nvs, "bridge_host", cfg->bridge_host);
    if (e == ESP_OK) e = nvs_set_u16(nvs, "bridge_port", cfg->bridge_port);
    if (e == ESP_OK) e = nvs_set_u8(nvs, "brightness", cfg->brightness);
    if (e == ESP_OK) e = nvs_set_u8(nvs, "card_opacity", cfg->card_opacity);
    if (e == ESP_OK) e = nvs_set_u16(nvs, "sleep_timeout_s", cfg->sleep_timeout_s);
    if (e == ESP_OK) e = nvs_set_u8(nvs, "provisioned", 1);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

esp_err_t app_config_save_player(const char *player_id) {
    if (!player_id || !player_id[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_str(nvs, "player_id", player_id);
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}
