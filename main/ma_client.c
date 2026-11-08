#include "ma_client.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ma_client";

#define PLAYERS_LIST_CAP  32768
#define QUEUE_RESP_CAP     8192
#define PLAYER_RESP_CAP    4096
#define COMMAND_RESP_CAP   2048

static char s_url[128];
static char s_auth[560];
static char s_player_id[96];
static unsigned long s_msg_id;
static int s_last_status;
static SemaphoreHandle_t s_ma_mutex;

static void copy_json_str(char *dst, size_t cap, const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, cap, "%s", item->valuestring);
    }
}

// The group queue response can exceed QUEUE_RESP_CAP because it embeds the
// queue items; queue_id is still in the leading bytes. Extract it before
// attempting to parse a possibly truncated JSON document.
static bool json_string_prefix(const char *buf, const char *key, char *out, size_t cap) {
    const char *p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    const size_t n = (size_t)(end - p);
    if (n == 0 || n >= cap) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static int json_int(const cJSON *obj, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

static esp_err_t ma_post(const char *command, const char *args,
                         char *resp, size_t cap, size_t *out_len) {
    char body[320];
    int n = snprintf(body, sizeof(body),
                     "{\"message_id\":\"%lu\",\"command\":\"%s\",\"args\":%s}",
                     ++s_msg_id, command, args);
    if (n < 0 || (size_t)n >= sizeof(body)) return ESP_ERR_INVALID_SIZE;
    if (s_ma_mutex) xSemaphoreTake(s_ma_mutex, portMAX_DELAY);

    const esp_http_client_config_t cfg = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 6000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (s_ma_mutex) xSemaphoreGive(s_ma_mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", s_auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = ESP_OK;
    bool opened = false;
    s_last_status = 0;
    if (esp_http_client_open(client, (int)strlen(body)) == ESP_OK) {
        opened = true;
        const int written = esp_http_client_write(client, body, (int)strlen(body));
        if (written != (int)strlen(body)) err = ESP_FAIL;
    } else {
        err = ESP_FAIL;
    }

    size_t total = 0;
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        s_last_status = esp_http_client_get_status_code(client);
        if (s_last_status == 200) {
            while (total + 1 < cap) {
                const int r = esp_http_client_read(client, resp + total,
                                                   (int)(cap - 1 - total));
                if (r <= 0) break;
                total += (size_t)r;
            }
            resp[total] = '\0';
            if (out_len) *out_len = total;
        } else {
            ESP_LOGW(TAG, "%s -> HTTP %d", command, s_last_status);
            err = (s_last_status == 401 || s_last_status == 403)
                      ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        }
    }

    if (opened) esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK && s_last_status == 0) {
        ESP_LOGW(TAG, "%s failed: %s", command, esp_err_to_name(err));
    }
    if (s_ma_mutex) xSemaphoreGive(s_ma_mutex);
    return err;
}

esp_err_t ma_client_init(const app_config_t *cfg) {
    if (!cfg || !cfg->ma_host[0] || !cfg->ma_token[0]) {
        ESP_LOGE(TAG, "missing MA host or token");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_url, sizeof(s_url), "http://%s:%u/api", cfg->ma_host, cfg->ma_port);
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", cfg->ma_token);
    snprintf(s_player_id, sizeof(s_player_id), "%s", cfg->player_id);
    if (!s_ma_mutex) s_ma_mutex = xSemaphoreCreateMutex();
    if (!s_ma_mutex) {
        ESP_LOGE(TAG, "MA request mutex allocation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MA endpoint %s, player %s", s_url, s_player_id);
    return ESP_OK;
}

esp_err_t ma_client_set_player(const char *player_id) {
    if (!player_id || !player_id[0]) return ESP_ERR_INVALID_ARG;
    snprintf(s_player_id, sizeof(s_player_id), "%s", player_id);
    return ESP_OK;
}

static bool json_true(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) && cJSON_IsTrue(item);
}

static bool json_false(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) && cJSON_IsFalse(item);
}

static bool player_usable(const cJSON *root) {
    const cJSON *available = cJSON_GetObjectItemCaseSensitive(root, "available");
    if (cJSON_IsBool(available) && !cJSON_IsTrue(available)) return false;
    if (json_true(root, "private") || json_true(root, "hide_in_ui")) return false;
    if (json_false(root, "enabled")) return false;
    return true;
}

static void player_parse(const cJSON *root, const char *player_id, ma_player_t *player,
                         char *ip_tail, size_t ip_cap) {
    memset(player, 0, sizeof(*player));
    snprintf(player->player_id, sizeof(player->player_id), "%s", player_id);
    copy_json_str(player->name, sizeof(player->name), root, "name");
    if (!player->name[0]) {
        snprintf(player->name, sizeof(player->name), "%s", player_id);
    }
    snprintf(player->display, sizeof(player->display), "%s", player->name);
    player->volume = -1;
    ip_tail[0] = '\0';

    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_info");
    const cJSON *identifiers = cJSON_GetObjectItemCaseSensitive(device, "identifiers");
    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(identifiers, "ip_address");
    if (cJSON_IsString(ip) && ip->valuestring) {
        const char *dot = strrchr(ip->valuestring, '.');
        snprintf(ip_tail, ip_cap, "%s", dot ? dot + 1 : ip->valuestring);
    }

    const cJSON *vol = cJSON_GetObjectItemCaseSensitive(root, "volume_level");
    if (cJSON_IsNumber(vol)) {
        player->volume = (int)vol->valuedouble;
        player->volume_known = true;
    } else {
        const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group_volume");
        if (cJSON_IsNumber(group) && group->valuedouble > 0) {
            player->volume = (int)group->valuedouble;
            player->volume_known = true;
        }
    }

    const cJSON *features = cJSON_GetObjectItemCaseSensitive(root, "supported_features");
    if (cJSON_IsArray(features)) {
        const cJSON *feature = NULL;
        cJSON_ArrayForEach(feature, features) {
            if (cJSON_IsString(feature) &&
                strcmp(feature->valuestring, "volume_set") == 0) {
                player->can_volume = true;
            }
        }
    }

    copy_json_str(player->state, sizeof(player->state), root, "playback_state");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *members = cJSON_GetObjectItemCaseSensitive(root, "group_members");
    player->is_group =
        (cJSON_IsString(type) && strcmp(type->valuestring, "group") == 0) ||
        cJSON_IsArray(members);
    const cJSON *media = cJSON_GetObjectItemCaseSensitive(root, "current_media");
    if (cJSON_IsObject(media)) {
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(media, "title");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(media, "name");
        player->has_media = (cJSON_IsString(title) && title->valuestring[0]) ||
                            (cJSON_IsString(name) && name->valuestring[0]);
    }
}

static esp_err_t ma_player_get(const char *player_id, ma_player_t *player, char *ip_tail,
                               size_t ip_cap) {
    char args[160];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\"}", player_id);
    char *buf = malloc(PLAYER_RESP_CAP);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = 0;
    esp_err_t err = ma_post("players/get", args, buf, PLAYER_RESP_CAP, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        free(buf);
        return ESP_FAIL;
    }
    if (!player_usable(root)) {
        cJSON_Delete(root);
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }
    player_parse(root, player_id, player, ip_tail, ip_cap);
    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}

static int scan_player_ids(const char *buf, char ids[MA_MAX_PLAYERS][96]) {
    int count = 0;
    const char *key = "\"player_id\"";
    const char *pos = buf;
    while ((pos = strstr(pos, key)) != NULL) {
        pos += strlen(key);
        while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
        if (*pos != ':') continue;
        pos++;
        while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
        if (*pos != '"') continue;
        pos++;
        const char *end = strchr(pos, '"');
        if (!end) break;
        const size_t len = (size_t)(end - pos);
        if (len > 0 && len < 96) {
            bool duplicate = false;
            for (int i = 0; i < count; i++) {
                if (strlen(ids[i]) == len && strncmp(ids[i], pos, len) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && count < MA_MAX_PLAYERS) {
                memcpy(ids[count], pos, len);
                ids[count][len] = '\0';
                count++;
            }
        }
        pos = end + 1;
    }
    return count;
}

esp_err_t ma_fetch_players(ma_players_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    char *buf = malloc(PLAYERS_LIST_CAP);
    if (!buf) {
        ESP_LOGE(TAG, "players/all alloc failed (%u bytes, heap %u)",
                 (unsigned)PLAYERS_LIST_CAP, esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    size_t len = 0;
    esp_err_t err = ma_post("players/all", "{\"return_unavailable\":false}",
                            buf, PLAYERS_LIST_CAP, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    char ids[MA_MAX_PLAYERS][96];
    const int id_count = scan_player_ids(buf, ids);
    free(buf);
    ESP_LOGI(TAG, "players/all: %d ids, %u bytes", id_count, (unsigned)len);

    char ip_tail[MA_MAX_PLAYERS][8];
    for (int i = 0; i < id_count && out->count < MA_MAX_PLAYERS; i++) {
        const esp_err_t get_err = ma_player_get(ids[i], &out->items[out->count],
                                                ip_tail[out->count],
                                                sizeof(ip_tail[0]));
        if (get_err == ESP_ERR_NOT_FOUND) continue;
        if (get_err != ESP_OK) {
            ESP_LOGW(TAG, "players/get %s failed: %s", ids[i], esp_err_to_name(get_err));
            continue;
        }
        out->count++;
    }

    for (int i = 0; i < out->count; i++) {
        bool duplicate = false;
        for (int j = 0; j < out->count; j++) {
            if (i != j && strcmp(out->items[i].name, out->items[j].name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate && ip_tail[i][0]) {
            char display[96];
            snprintf(display, sizeof(display), "%s · %s", out->items[i].name, ip_tail[i]);
            snprintf(out->items[i].display, sizeof(out->items[i].display), "%s", display);
        }
        ESP_LOGI(TAG, "player %s (%s) volume=%d%s", out->items[i].display,
                 out->items[i].player_id, out->items[i].volume,
                 out->items[i].volume_known ? "" : "?");
    }

    return out->count > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ma_fetch_queue_id(char *queue_id, size_t cap) {
    if (!queue_id || cap == 0) return ESP_ERR_INVALID_ARG;
    queue_id[0] = '\0';

    char args[128];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\"}", s_player_id);
    char *buf = malloc(QUEUE_RESP_CAP);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = 0;
    esp_err_t err = ma_post("player_queues/get_active_queue", args,
                            buf, QUEUE_RESP_CAP, &len);
    if (err == ESP_OK) {
        if (!json_string_prefix(buf, "\"queue_id\"", queue_id, cap)) {
            cJSON *root = cJSON_Parse(buf);
            if (cJSON_IsObject(root)) {
                copy_json_str(queue_id, cap, root, "queue_id");
            }
            cJSON_Delete(root);
        }
        if (queue_id[0]) {
            ESP_LOGI(TAG, "active queue '%s' (%u bytes)", queue_id, (unsigned)len);
        } else {
            err = ESP_FAIL;
        }
    }
    free(buf);
    return err;
}

esp_err_t ma_fetch_status(ma_status_t *st) {
    if (!st) return ESP_ERR_INVALID_ARG;
    memset(st, 0, sizeof(*st));
    st->volume = -1;

    char args[128];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\"}", s_player_id);
    char *buf = malloc(PLAYER_RESP_CAP);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t len = 0;
    esp_err_t err = ma_post("players/get", args, buf, PLAYER_RESP_CAP, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "players/get parse failed (%u bytes)", (unsigned)len);
        cJSON_Delete(root);
        free(buf);
        return ESP_FAIL;
    }

    copy_json_str(st->player_name, sizeof(st->player_name), root, "name");
    copy_json_str(st->state, sizeof(st->state), root, "playback_state");

    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(root, "volume_level");
    if (cJSON_IsNumber(volume)) {
        st->volume = (int)volume->valuedouble;
        st->volume_known = true;
    } else {
        const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "group_volume");
        if (cJSON_IsNumber(group) && group->valuedouble > 0) {
            st->volume = (int)group->valuedouble;
            st->volume_known = true;
        }
    }

    const cJSON *features = cJSON_GetObjectItemCaseSensitive(root, "supported_features");
    if (cJSON_IsArray(features)) {
        const cJSON *f = NULL;
        cJSON_ArrayForEach(f, features) {
            if (cJSON_IsString(f) && strcmp(f->valuestring, "volume_set") == 0) {
                st->can_volume = true;
            }
        }
    }

    const cJSON *media = cJSON_GetObjectItemCaseSensitive(root, "current_media");
    if (cJSON_IsObject(media)) {
        copy_json_str(st->title, sizeof(st->title), media, "title");
        copy_json_str(st->artist, sizeof(st->artist), media, "artist");
        copy_json_str(st->album, sizeof(st->album), media, "album");
        st->duration_s = json_int(media, "duration", 0);
        st->elapsed_s = json_int(media, "elapsed_time", 0);
        if (!st->title[0]) {
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(media, "media_item");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (cJSON_IsString(name)) snprintf(st->title, sizeof(st->title), "%s", name->valuestring);
        }
    }
    st->playing = strcmp(st->state, "playing") == 0;
    st->valid = true;

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}

int ma_last_http_status(void) {
    return s_last_status;
}

static esp_err_t ma_call(const char *command, const char *args) {
    char resp[COMMAND_RESP_CAP];
    size_t len = 0;
    const esp_err_t err = ma_post(command, args, resp, sizeof(resp), &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s ok (%u bytes)", command, (unsigned)len);
    } else {
        ESP_LOGW(TAG, "%s failed: %s (HTTP %d)", command, esp_err_to_name(err),
                 ma_last_http_status());
    }
    return err;
}

esp_err_t ma_play_pause(void) {
    char args[160];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\"}", s_player_id);
    return ma_call("players/cmd/play_pause", args);
}

esp_err_t ma_volume_set(int volume) {
    char args[192];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\",\"volume_level\":%d}",
             s_player_id, volume);
    return ma_call("players/cmd/volume_set", args);
}

esp_err_t ma_volume_up(void) {
    char args[160];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\"}", s_player_id);
    return ma_call("players/cmd/volume_up", args);
}

esp_err_t ma_volume_down(void) {
    char args[160];
    snprintf(args, sizeof(args), "{\"player_id\":\"%s\"}", s_player_id);
    return ma_call("players/cmd/volume_down", args);
}

esp_err_t ma_queue_next(const char *queue_id) {
    char args[128];
    snprintf(args, sizeof(args), "{\"queue_id\":\"%s\"}", queue_id);
    return ma_call("player_queues/next", args);
}

esp_err_t ma_queue_previous(const char *queue_id) {
    char args[128];
    snprintf(args, sizeof(args), "{\"queue_id\":\"%s\"}", queue_id);
    return ma_call("player_queues/previous", args);
}
