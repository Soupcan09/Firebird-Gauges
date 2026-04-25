/*
 * Settings.h -- persistent, user-adjustable settings for the Firebird
 * gauge. Values are stored in NVS (non-volatile flash) so they survive
 * power cycles, and cached in RAM so read hot-path calls are free.
 *
 * Settings:
 *   OVERHEAT TRIP (F)  -- temp at which the red OVERHEAT banner trips.
 *                         Clear threshold is always TRIP - HYST (4 F).
 *   TEMP OFFSET   (F)  -- bias applied to the resistance-to-temp lookup
 *                         output. Use this to dial in the gauge against
 *                         an IR thermometer on the thermostat housing.
 *   BRIGHTNESS    (%)  -- LCD backlight, 20-100. Hooked to Set_Backlight.
 *   BUZZER        (bool)- runtime enable for the alarm chirp. Replaces
 *                         the compile-time ALARM_BUZZER_ENABLE flag.
 *
 * Call Settings_Init() from app_main() AFTER Wireless_Init() (which
 * initializes NVS) and BEFORE TempSender_Init() or show_gauge().
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds and defaults -- exposed so the settings UI can display and
 * enforce the same ranges. */
#define SETTINGS_TRIP_F_MIN         195.0f
#define SETTINGS_TRIP_F_MAX         225.0f
#define SETTINGS_TRIP_F_DEFAULT     205.0f
#define SETTINGS_TRIP_F_STEP          1.0f

#define SETTINGS_HYST_F               4.0f   /* fixed, not user-adjustable */

#define SETTINGS_OFFSET_F_MIN       -10.0f
#define SETTINGS_OFFSET_F_MAX        10.0f
#define SETTINGS_OFFSET_F_DEFAULT     0.0f
#define SETTINGS_OFFSET_F_STEP        1.0f

#define SETTINGS_BRIGHT_MIN            20
#define SETTINGS_BRIGHT_MAX           100
#define SETTINGS_BRIGHT_DEFAULT        70
#define SETTINGS_BRIGHT_STEP           10

/* Splash screen hold time, in seconds. Range 1-10 with 1 s steps; the
 * default 3 s is the previous compile-time SPLASH_HOLD_MS = 3000.
 * Used by splash_screen.c as the full-opacity hold duration before
 * the fade-out begins. */
#define SETTINGS_SPLASH_S_MIN           1
#define SETTINGS_SPLASH_S_MAX          10
#define SETTINGS_SPLASH_S_DEFAULT       3
#define SETTINGS_SPLASH_S_STEP          1

/* Init: load all settings from NVS into the RAM cache, or defaults if
 * none are stored yet. Also applies hardware side effects for values
 * that need them (currently: sets LCD backlight). */
void Settings_Init(void);

/* Restore every setting to its default, persist to NVS, and reapply
 * side effects. Called from the RESET DEFAULTS button on the UI. */
void Settings_ResetDefaults(void);

/* --- Getters (read cached RAM value, safe from any task) ---------- */
float   Settings_GetOverheatTripF(void);
float   Settings_GetOverheatClearF(void);      /* trip - HYST, derived */
float   Settings_GetTempOffsetF(void);
uint8_t Settings_GetBrightness(void);
bool    Settings_GetBuzzerEnabled(void);
uint8_t Settings_GetSplashTimeS(void);

/* --- Setters (clamp, write NVS, update RAM, apply side effect) ---- */
void Settings_SetOverheatTripF(float v);
void Settings_SetTempOffsetF(float v);
void Settings_SetBrightness(uint8_t v);
void Settings_SetBuzzerEnabled(bool on);
void Settings_SetSplashTimeS(uint8_t v);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
