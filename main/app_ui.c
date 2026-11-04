#include "app_ui.h"

#include "bsp_battery.h"
#include "lvgl.h"

LV_FONT_DECLARE(app_font_20);
LV_FONT_DECLARE(app_font_16);

#define UI_BG        0x0B0F14
#define UI_CARD      0x151C26
#define UI_LINE      0x232B36
#define UI_TRACK     0x2A3441
#define UI_TEXT      0xF2F5F8
#define UI_MUTED     0x8A97A6
#define UI_FAINT     0x5A6675
#define UI_ACCENT    0x4CC2FF
#define UI_PLAYING   0x63D08B
#define UI_ERROR     0xE86A6A

#define FONT_TITLE &app_font_20
#define FONT_BODY  &app_font_16
#define FONT_ICON  &lv_font_montserrat_14

#define WAVE_BARS 9
#define WAVE_BASELINE 110
#define WAVE_MIN_H 4
#define WAVE_MAX_EXTRA 9

static lv_obj_t *s_state;
static lv_obj_t *s_title;
static lv_obj_t *s_artist;
static lv_obj_t *s_player;
static lv_obj_t *s_progress;
static lv_obj_t *s_elapsed;
static lv_obj_t *s_duration;
static lv_obj_t *s_volume_value;
static lv_obj_t *s_volume_bar;
static lv_obj_t *s_battery;
static lv_obj_t *s_wifi;
static lv_obj_t *s_wave[WAVE_BARS];

static const uint8_t k_wave_curve[WAVE_BARS] = { 45, 70, 100, 80, 95, 60, 85, 75, 50 };

static void wave_show(bool show) {
    for (int i = 0; i < WAVE_BARS; i++) {
        if (!s_wave[i]) continue;
        if (show) lv_obj_remove_flag(s_wave[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_wave[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *card_create(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    return card;
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *bar_create(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return bar;
}

static void set_time(lv_obj_t *label, int seconds, const char *fallback) {
    if (seconds <= 0) {
        lv_label_set_text(label, fallback);
    } else {
        lv_label_set_text_fmt(label, "%d:%02d", seconds / 60, seconds % 60);
    }
}

static void set_playing_icon(bool playing) {
    lv_label_set_text(s_state, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_state, lv_color_hex(playing ? UI_PLAYING : UI_FAINT), 0);
}

static void battery_tick(lv_timer_t *timer) {
    (void)timer;
    const int soc = bsp_battery_soc();
    const char *symbol = LV_SYMBOL_BATTERY_EMPTY;
    if (soc >= 90) symbol = LV_SYMBOL_BATTERY_FULL;
    else if (soc >= 65) symbol = LV_SYMBOL_BATTERY_3;
    else if (soc >= 40) symbol = LV_SYMBOL_BATTERY_2;
    else if (soc >= 15) symbol = LV_SYMBOL_BATTERY_1;
    lv_label_set_text(s_battery, symbol);
    lv_obj_set_style_text_color(s_battery,
                                lv_color_hex(soc >= 0 && soc < 15 ? UI_ERROR : UI_MUTED), 0);
}

void app_ui_init(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *brand = label_create(scr, LV_SYMBOL_AUDIO, FONT_ICON, UI_ACCENT);
    lv_obj_set_pos(brand, 14, 8);
    s_wifi = label_create(scr, LV_SYMBOL_WIFI, FONT_ICON, UI_FAINT);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -34, 8);
    s_battery = label_create(scr, LV_SYMBOL_BATTERY_EMPTY, FONT_ICON, UI_MUTED);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -14, 8);

    lv_obj_t *divider = lv_obj_create(scr);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(divider, 14, 34);
    lv_obj_set_size(divider, 212, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);

    lv_obj_t *now = card_create(scr, 10, 42, 220, 152, 14);
    s_state = label_create(now, LV_SYMBOL_PLAY, FONT_ICON, UI_FAINT);
    lv_obj_align(s_state, LV_ALIGN_TOP_RIGHT, -14, 12);

    s_title = label_create(now, "正在连接", FONT_TITLE, UI_TEXT);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_title, 14, 14);
    lv_obj_set_size(s_title, 192, 48);

    s_artist = label_create(now, "Music Assistant", FONT_BODY, UI_MUTED);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_artist, 14, 74);
    lv_obj_set_width(s_artist, 192);

    s_progress = bar_create(now, 14, 112, 192, 8);
    s_elapsed = label_create(now, "0:00", FONT_BODY, UI_FAINT);
    lv_obj_set_pos(s_elapsed, 14, 124);
    s_duration = label_create(now, "--:--", FONT_BODY, UI_FAINT);
    lv_obj_align(s_duration, LV_ALIGN_TOP_RIGHT, -14, 124);

    for (int i = 0; i < WAVE_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(now);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(bar, 72 + i * 9, WAVE_BASELINE - WAVE_MIN_H);
        lv_obj_set_size(bar, 4, WAVE_MIN_H);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(UI_ACCENT), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        s_wave[i] = bar;
    }

    lv_obj_t *volume = card_create(scr, 10, 204, 220, 56, 14);
    lv_obj_t *volume_icon = label_create(volume, LV_SYMBOL_VOLUME_MAX, FONT_ICON, UI_MUTED);
    lv_obj_set_pos(volume_icon, 14, 12);
    s_volume_value = label_create(volume, "--", FONT_BODY, UI_TEXT);
    lv_obj_align(s_volume_value, LV_ALIGN_TOP_RIGHT, -14, 10);
    s_volume_bar = bar_create(volume, 14, 38, 192, 6);
    lv_bar_set_value(s_volume_bar, 0, LV_ANIM_OFF);

    s_player = label_create(scr, "", FONT_BODY, UI_MUTED);
    lv_label_set_long_mode(s_player, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_player, 14, 274);
    lv_obj_set_width(s_player, 212);

    lv_screen_load(scr);
    lv_timer_create(battery_tick, 5000, NULL);
    battery_tick(NULL);
}

void app_ui_set_wifi(bool connected) {
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(connected ? UI_PLAYING : UI_FAINT), 0);
}

void app_ui_set_playing(bool playing) {
    set_playing_icon(playing);
}

void app_ui_set_track(const char *title, const char *subtitle, const char *player,
                      bool playing, int elapsed_s, int duration_s) {
    wave_show(false);
    lv_label_set_text(s_title, title && title[0] ? title : "暂无播放");
    lv_label_set_text(s_artist, subtitle && subtitle[0] ? subtitle : "");
    lv_label_set_text(s_player, player ? player : "");
    set_playing_icon(playing);
    if (duration_s > 0 && elapsed_s > duration_s) elapsed_s = duration_s;
    const int percent = duration_s > 0 ? elapsed_s * 100 / duration_s : 0;
    lv_bar_set_value(s_progress, percent < 0 ? 0 : (percent > 100 ? 100 : percent),
                     LV_ANIM_OFF);
    set_time(s_elapsed, elapsed_s, "0:00");
    set_time(s_duration, duration_s, "--:--");
}

void app_ui_set_message(const char *headline, const char *detail) {
    wave_show(false);
    lv_label_set_text(s_title, headline ? headline : "");
    lv_label_set_text(s_artist, detail ? detail : "");
    set_playing_icon(false);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_label_set_text(s_elapsed, "0:00");
    lv_label_set_text(s_duration, "--:--");
}

void app_ui_set_volume(int volume) {
    app_ui_set_volume_ex(volume, false);
}

void app_ui_set_volume_ex(int volume, bool estimated) {
    if (volume < 0) {
        lv_label_set_text(s_volume_value, "--");
        lv_bar_set_value(s_volume_bar, 0, LV_ANIM_OFF);
        return;
    }
    if (volume > 100) volume = 100;
    lv_bar_set_value(s_volume_bar, volume, LV_ANIM_OFF);
    if (estimated) lv_label_set_text_fmt(s_volume_value, "~%d", volume);
    else lv_label_set_text_fmt(s_volume_value, "%d", volume);
}

void app_ui_set_listening(bool listening) {
    wave_show(listening);
    if (listening) app_ui_set_audio_level(0);
}

void app_ui_set_audio_level(int level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    for (int i = 0; i < WAVE_BARS; i++) {
        if (!s_wave[i]) continue;
        const int extra = level * k_wave_curve[i] * WAVE_MAX_EXTRA / 10000;
        const int h = WAVE_MIN_H + extra;
        lv_obj_set_size(s_wave[i], 4, h);
        lv_obj_set_pos(s_wave[i], 72 + i * 9, WAVE_BASELINE - h);
    }
}

void app_ui_set_ota_progress(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    wave_show(false);
    lv_label_set_text(s_title, "正在升级");
    lv_label_set_text(s_artist, "请勿断电");
    lv_label_set_text(s_player, "");
    set_playing_icon(false);
    lv_bar_set_value(s_progress, percent, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_elapsed, "%d%%", percent);
    lv_label_set_text(s_duration, "");
}

void app_ui_set_provisioning(const char *ap_ssid) {
    wave_show(false);
    lv_label_set_text(s_title, "配网模式");
    lv_label_set_text_fmt(s_artist, "连接热点 %s", ap_ssid ? ap_ssid : "");
    lv_label_set_text(s_player, "打开 192.168.4.1");
    set_playing_icon(false);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_label_set_text(s_elapsed, "0:00");
    lv_label_set_text(s_duration, "--:--");
}
