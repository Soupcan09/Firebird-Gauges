/*
 * splash_screen.h -- Startup splash screen for the Firebird gauge.
 *
 * show_splash() puts a full-screen splash image up immediately and
 * starts a one-shot timer. When the timer expires it calls show_gauge()
 * to transition to the live gauge screen. Call once from app_main
 * after LVGL_Init().
 */
#ifndef SPLASH_SCREEN_H
#define SPLASH_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Display the splash. After the hold duration it auto-transitions to
 * the gauge screen via show_gauge(). */
void show_splash(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLASH_SCREEN_H */
