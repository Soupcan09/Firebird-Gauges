/*
 * display_gauge.h -- Firebird water temp gauge screen.
 */
#ifndef DISPLAY_GAUGE_H
#define DISPLAY_GAUGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build and activate the gauge screen. Call once, after LVGL init. */
void show_gauge(void);

/* Move the needle to a temperature (Fahrenheit). Clamped to the dial
 * range 100-260 F. Call from your sensor read loop once we wire the
 * GM sender. With GAUGE_DEMO_SWEEP enabled in display_gauge.c, the
 * sweep timer will fight this -- disable it first. */
void set_gauge_temp_f(float temp_f);

/* Show/hide a flashing red alarm banner with custom message text.
 *   active=true  -> banner visible, flashing at 2 Hz, showing `message`
 *   active=false -> banner hidden (message ignored)
 * Used for BOTH sensor-fault ("CHECK SENSOR") and overheat alarms
 * ("OVERHEAT"). Caller keeps its own state machine and calls this
 * every sample tick; the setter is idempotent when nothing changes. */
void set_gauge_alarm(bool active, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_GAUGE_H */
