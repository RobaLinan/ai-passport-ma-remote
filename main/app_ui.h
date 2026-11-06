#pragma once

#include <stdbool.h>

// All functions here touch LVGL objects: call them with bsp_lvgl_lock() held.
void app_ui_init(void);
void app_ui_set_wifi(bool connected);
void app_ui_set_playing(bool playing);
void app_ui_set_track(const char *title, const char *subtitle, const char *player,
                      bool playing, int elapsed_s, int duration_s);
void app_ui_set_message(const char *headline, const char *detail);
void app_ui_set_volume(int volume);
void app_ui_set_volume_ex(int volume, bool estimated);
void app_ui_set_listening(bool listening);
void app_ui_set_audio_level(int level);
void app_ui_set_ota_progress(int percent);
void app_ui_set_card_opacity(int percent);
void app_ui_set_provisioning(const char *ap_ssid);
