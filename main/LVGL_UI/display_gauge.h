/*
 * display_gauge.h -- Firebird water temp gauge screen.
 *
 * Single-face Pontiac-themed ole-school gauge:
 *   - 100..260 F full range, 180 F at 12 o'clock
 *   - Baked red warning band 240..260
 *   - Live needle, pivot hub, digital readout, and optional live red
 *     overheat arc drawn by LVGL on top of the face artwork.
 *
 * The red overheat arc follows Settings_GetOverheatTripF() so the visual
 * warning extends from the user's alarm setpoint down to the baked 240 F
 * factory band whenever the trip is set below 240.
 */
#ifndef DISPLAY_GAUGE_H
#define DISPLAY_GAUGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build and activate the gauge screen. Safe to call repeatedly -- each
 * call builds a fresh screen from scratch and swaps it in. */
void show_gauge(void);

/* Move the needle to a temperature (Fahrenheit). Clamped to 100..260 F.
 * Also repaints the live digital readout. No-op if the gauge screen is
 * not currently built (e.g. settings is showing). */
void set_gauge_temp_f(float temp_f);

/* Show/hide a flashing red alarm banner with custom message text.
 *   active=true  -> banner visible, flashing at 2 Hz, showing `message`
 *   active=false -> banner hidden (message ignored)
 * Used for BOTH sensor-fault ("CHECK SENSOR") and overheat alarms
 * ("OVERHEAT"). Caller keeps its own state machine and calls this every
 * sample tick; the setter is idempotent when nothing changes. */
void set_gauge_alarm(bool active, const char *message);

/* Invalidate the gauge's widget references. Call this BEFORE destroying
 * the gauge screen (e.g. from the settings screen) so the Temp_Sender
 * task's continuous set_gauge_temp_f() / set_gauge_alarm() calls become
 * no-ops instead of dereferencing freed LVGL objects. */
void gauge_release(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_GAUGE_H */
