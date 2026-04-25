/*
 * settings_screen.h -- on-screen settings UI for the Firebird gauge.
 *
 * Entered by long-pressing anywhere on the gauge screen (see display_gauge
 * for the trigger). Lets the driver adjust:
 *     OVERHEAT trip point, TEMP offset, BRIGHTNESS, BUZZER enable
 * All changes write through to Settings / NVS immediately and are applied
 * live. Tapping BACK returns to the gauge.
 */
#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build and activate the settings screen on the current display. */
void show_settings(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_SCREEN_H */
