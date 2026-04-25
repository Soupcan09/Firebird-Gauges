/*
 * Settings.c -- NVS-backed storage + RAM cache for user settings.
 * See Settings.h for the public API and bounds.
 *
 * Storage layout: one NVS namespace "firebird", one key per setting.
 * Floats are stored via memcpy into int32 to keep using nvs_get/set_i32
 * (NVS has no native float type). That's safe because sizeof(float) ==
 * sizeof(int32_t) on ESP32 and memcpy avoids strict-aliasing problems.
 *
 * NVS writes are rare (only when the user taps +/- in the UI) so we
 * commit on every setter. Flash wear is a non-issue.
 */
#include "Settings.h"

#include <string.h>

#include "nvs.h"
#include "esp_log.h"
#include "ST7701S.h"        /* Set_Backlight(uint8_t) */

static const char *TAG = "Settings";

#define NVS_NAMESPACE   "firebird"
#define KEY_TRIP_F      "trip_f"
#define KEY_OFFSET_F    "offset_f"
#define KEY_BRIGHT      "bright"
#define KEY_BUZZER      "buzzer"
#define KEY_SPLASH_S    "splash_s"
/* KEY_STYLE retired with the single-face Pontiac redesign; any existing
 * "style" entry in NVS is harmless -- it's just ignored. */

/* RAM cache -- getters return these, setters update these + flush. */
static float   s_trip_f   = SETTINGS_TRIP_F_DEFAULT;
static float   s_offset_f = SETTINGS_OFFSET_F_DEFAULT;
static uint8_t s_bright   = SETTINGS_BRIGHT_DEFAULT;
static bool    s_buzzer   = false;
static uint8_t s_splash_s = SETTINGS_SPLASH_S_DEFAULT;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint8_t clamp_u8(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)v;
}

/* Read a float stored as an int32 bit-pattern. Leaves `*out` unchanged
 * if the key is missing, so the caller's default survives. */
static void load_float(nvs_handle_t h, const char *k, float *out)
{
    int32_t raw = 0;
    if (nvs_get_i32(h, k, &raw) == ESP_OK) {
        memcpy(out, &raw, sizeof(*out));
    }
}

/* Open handle, write float as i32, commit, close. Returns true on
 * full success. */
static bool save_float(const char *k, float v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    int32_t raw;
    memcpy(&raw, &v, sizeof(raw));
    esp_err_t e1 = nvs_set_i32(h, k, raw);
    esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    return (e1 == ESP_OK) && (e2 == ESP_OK);
}

static bool save_u8(const char *k, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_set_u8(h, k, v);
    esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    return (e1 == ESP_OK) && (e2 == ESP_OK);
}

/* ------------------------------------------------------------------ */
/* Init / reset                                                        */
/* ------------------------------------------------------------------ */
void Settings_Init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (err=%d), using compiled defaults", err);
    } else {
        load_float(h, KEY_TRIP_F,   &s_trip_f);
        load_float(h, KEY_OFFSET_F, &s_offset_f);

        uint8_t u8 = s_bright;
        if (nvs_get_u8(h, KEY_BRIGHT, &u8) == ESP_OK) s_bright = u8;

        uint8_t bz = 0;
        if (nvs_get_u8(h, KEY_BUZZER, &bz) == ESP_OK) s_buzzer = (bz != 0);

        uint8_t sp = s_splash_s;
        if (nvs_get_u8(h, KEY_SPLASH_S, &sp) == ESP_OK) s_splash_s = sp;

        nvs_close(h);
    }

    /* Clamp in case stored values predate a bound change (or flash
     * got weird). */
    s_trip_f   = clampf(s_trip_f,   SETTINGS_TRIP_F_MIN,   SETTINGS_TRIP_F_MAX);
    s_offset_f = clampf(s_offset_f, SETTINGS_OFFSET_F_MIN, SETTINGS_OFFSET_F_MAX);
    s_bright   = clamp_u8(s_bright, SETTINGS_BRIGHT_MIN,   SETTINGS_BRIGHT_MAX);
    s_splash_s = clamp_u8(s_splash_s, SETTINGS_SPLASH_S_MIN, SETTINGS_SPLASH_S_MAX);

    /* Side effect: push brightness to the LCD right now. */
    Set_Backlight(s_bright);

    ESP_LOGI(TAG, "loaded: trip=%.0fF offset=%+.0fF bright=%u buzzer=%s splash=%us",
             s_trip_f, s_offset_f, s_bright, s_buzzer ? "on" : "off", s_splash_s);
}

void Settings_ResetDefaults(void)
{
    Settings_SetOverheatTripF(SETTINGS_TRIP_F_DEFAULT);
    Settings_SetTempOffsetF(SETTINGS_OFFSET_F_DEFAULT);
    Settings_SetBrightness(SETTINGS_BRIGHT_DEFAULT);
    Settings_SetBuzzerEnabled(false);
    Settings_SetSplashTimeS(SETTINGS_SPLASH_S_DEFAULT);
    ESP_LOGI(TAG, "reset to defaults");
}

/* ------------------------------------------------------------------ */
/* Getters                                                             */
/* ------------------------------------------------------------------ */
float   Settings_GetOverheatTripF(void)   { return s_trip_f; }
float   Settings_GetOverheatClearF(void)  { return s_trip_f - SETTINGS_HYST_F; }
float   Settings_GetTempOffsetF(void)     { return s_offset_f; }
uint8_t Settings_GetBrightness(void)      { return s_bright; }
bool    Settings_GetBuzzerEnabled(void)   { return s_buzzer; }
uint8_t Settings_GetSplashTimeS(void)     { return s_splash_s; }

/* ------------------------------------------------------------------ */
/* Setters                                                             */
/* ------------------------------------------------------------------ */
void Settings_SetOverheatTripF(float v)
{
    v = clampf(v, SETTINGS_TRIP_F_MIN, SETTINGS_TRIP_F_MAX);
    if (v == s_trip_f) return;
    s_trip_f = v;
    if (!save_float(KEY_TRIP_F, v)) {
        ESP_LOGW(TAG, "failed to persist trip=%.0fF", v);
    }
}

void Settings_SetTempOffsetF(float v)
{
    v = clampf(v, SETTINGS_OFFSET_F_MIN, SETTINGS_OFFSET_F_MAX);
    if (v == s_offset_f) return;
    s_offset_f = v;
    if (!save_float(KEY_OFFSET_F, v)) {
        ESP_LOGW(TAG, "failed to persist offset=%+.0fF", v);
    }
}

void Settings_SetBrightness(uint8_t v)
{
    v = clamp_u8(v, SETTINGS_BRIGHT_MIN, SETTINGS_BRIGHT_MAX);
    if (v == s_bright) return;
    s_bright = v;
    if (!save_u8(KEY_BRIGHT, v)) {
        ESP_LOGW(TAG, "failed to persist brightness=%u", v);
    }
    Set_Backlight(v);
}

void Settings_SetBuzzerEnabled(bool on)
{
    if (on == s_buzzer) return;
    s_buzzer = on;
    if (!save_u8(KEY_BUZZER, on ? 1 : 0)) {
        ESP_LOGW(TAG, "failed to persist buzzer=%d", on);
    }
}

void Settings_SetSplashTimeS(uint8_t v)
{
    v = clamp_u8(v, SETTINGS_SPLASH_S_MIN, SETTINGS_SPLASH_S_MAX);
    if (v == s_splash_s) return;
    s_splash_s = v;
    if (!save_u8(KEY_SPLASH_S, v)) {
        ESP_LOGW(TAG, "failed to persist splash_s=%u", v);
    }
}
