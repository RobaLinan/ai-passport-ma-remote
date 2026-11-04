#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Starts the LAN OTA web server after STA WiFi is connected.
esp_err_t ota_server_start(void);

// True while a firmware upload/write is in progress; blocks idle sleep.
bool ota_server_busy(void);
