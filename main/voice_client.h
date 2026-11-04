#pragma once

#include <stdbool.h>
#include "app_config.h"
#include "esp_err.h"

esp_err_t voice_client_init(const app_config_t *cfg);
void voice_client_start(const char *player_id, const char *queue_id);
void voice_client_listening(void);
void voice_client_commit(void);
void voice_client_stop(void);
void voice_client_cancel(void);
bool voice_client_busy(void);
bool voice_client_ui_blocked(void);
