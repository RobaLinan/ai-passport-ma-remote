#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_STA_IDLE = 0,
    WIFI_STA_CONNECTING,
    WIFI_STA_CONNECTED,
    WIFI_STA_FAILED,
} wifi_sta_state_t;

esp_err_t wifi_sta_start(const char *ssid, const char *pass);
wifi_sta_state_t wifi_sta_state(void);
void wifi_sta_set_power_save(bool enabled);
