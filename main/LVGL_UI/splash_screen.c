/*
 * splash_screen.c -- boot splash: 480x480 branded image held for
 * SPLASH_HOLD_MS milliseconds, then fades out into the gauge.
 *
 * Uses two LVGL one-shot timers:
 *   1) fade_cb   -- runs during the fade-out, decreasing image opacity
 *   2) done_cb   -- runs once at the very end, calls show_gauge()
 */
#include "lvgl.h"
#include "esp_log.h"
#include "splash_screen.h"
#include "display_gauge.h"
#include "splash_img.h"
#include "Settings.h"

static const char *TAG = "SPLASH";

/* Timings (milliseconds). The full-opacity hold duration is now driven
 * by the user-adjustable Settings_GetSplashTimeS() setting (1-10 s) so
 * Jeff can tune how long the boot logo lingers. Fade duration and step
 * cadence remain compile-time -- they're cosmetic, not user-facing. */
#define SPLASH_FADE_MS      400   /* fade-out duration after the hold     */
#define SPLASH_FADE_STEP_MS  25   /* fade animation tick period           */

static lv_obj_t   *s_splash_img = NULL;
static lv_timer_t *s_fade_timer = NULL;

/* --- one-shot: tear down splash and bring up the gauge --- */
static void splash_done_cb(lv_timer_t *t)
{
    (void)t;
    /* show_gauge() builds a fresh screen from scratch and swaps it in,
     * which destroys our splash image. */
    ESP_LOGI(TAG, "splash_done_cb -> show_gauge()");
    show_gauge();
    s_splash_img = NULL;
    s_fade_timer = NULL;
}

/* --- during fade-out: step opacity toward 0, then schedule done --- */
static void splash_fade_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_splash_img) {
        lv_timer_del(s_fade_timer);
        s_fade_timer = NULL;
        return;
    }

    static int elapsed_ms = 0;
    elapsed_ms += SPLASH_FADE_STEP_MS;

    int opa = LV_OPA_COVER -
              (LV_OPA_COVER * elapsed_ms) / SPLASH_FADE_MS;
    if (opa < 0) opa = 0;
    lv_obj_set_style_img_opa(s_splash_img, (lv_opa_t)opa, 0);

    if (elapsed_ms >= SPLASH_FADE_MS) {
        elapsed_ms = 0;
        lv_timer_del(s_fade_timer);
        s_fade_timer = NULL;
        /* Next LVGL tick, swap to the gauge screen */
        lv_timer_t *done = lv_timer_create(splash_done_cb, 10, NULL);
        lv_timer_set_repeat_count(done, 1);
    }
}

/* --- one-shot: start the fade-out after the hold --- */
static void splash_hold_done_cb(lv_timer_t *t)
{
    (void)t;
    if (s_fade_timer == NULL) {
        s_fade_timer = lv_timer_create(splash_fade_cb,
                                       SPLASH_FADE_STEP_MS, NULL);
    }
}

void show_splash(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_splash_img = lv_img_create(scr);
    lv_img_set_src(s_splash_img, &splash_img);
    lv_obj_align(s_splash_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_img_opa(s_splash_img, LV_OPA_COVER, 0);

    /* After the user-configured splash hold (Settings_GetSplashTimeS,
     * 1-10 s), begin fading out. */
    uint32_t hold_ms = (uint32_t)Settings_GetSplashTimeS() * 1000U;
    ESP_LOGI(TAG, "splash hold = %u ms", (unsigned)hold_ms);
    lv_timer_t *hold = lv_timer_create(splash_hold_done_cb, hold_ms, NULL);
    lv_timer_set_repeat_count(hold, 1);
}
