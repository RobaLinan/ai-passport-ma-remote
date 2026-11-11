#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char ma_host[64];
    uint16_t ma_port;
    char ma_token[512];
    char player_id[96];
    char bridge_host[64];
    uint16_t bridge_port;
    uint8_t brightness;
    uint8_t card_opacity;
    int8_t battery_offset;
    uint16_t sleep_timeout_s;
    bool provisioned;
} app_config_t;

// Loads settings from NVS; on first boot seeds NVS from main/secrets.h.
esp_err_t app_config_load(app_config_t *cfg);
esp_err_t app_config_save_all(const app_config_t *cfg);
esp_err_t app_config_save_player(const char *player_id);
