#pragma once

#include "app_config.h"
#include "esp_err.h"
#include <stdbool.h>

#define MA_MAX_PLAYERS 16

typedef struct {
    char player_id[96];
    char name[64];
    char display[96];
    char state[16];
    int volume;
    bool volume_known;
    bool can_volume;
    bool has_media;
    bool is_group;
} ma_player_t;

typedef struct {
    int count;
    ma_player_t items[MA_MAX_PLAYERS];
} ma_players_t;

typedef struct {
    bool valid;
    char player_name[64];
    char title[128];
    char artist[96];
    char album[96];
    char state[16];
    char queue_id[96];
    int duration_s;
    int elapsed_s;
    int volume;      // -1 when the player does not report one
    bool volume_known;
    bool playing;
    bool can_volume;
} ma_status_t;

esp_err_t ma_client_init(const app_config_t *cfg);
esp_err_t ma_client_set_player(const char *player_id);
esp_err_t ma_fetch_players(ma_players_t *out);
esp_err_t ma_fetch_queue_id(char *queue_id, size_t cap);
esp_err_t ma_fetch_status(ma_status_t *st);
esp_err_t ma_play_pause(void);
esp_err_t ma_volume_set(int volume);
esp_err_t ma_volume_up(void);
esp_err_t ma_volume_down(void);
esp_err_t ma_queue_next(const char *queue_id);
esp_err_t ma_queue_previous(const char *queue_id);
int ma_last_http_status(void);
