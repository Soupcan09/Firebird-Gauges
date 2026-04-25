/*
 * display_gauge.c
 *
 * Firebird water temp gauge screen. Single Pontiac-themed face:
 *   100..260 F full range, 180 F straight up, baked red band 240..260.
 *
 * Live widgets drawn on top of the face artwork:
 *   - Red overheat arc (tracks Settings_GetOverheatTripF())
 *   - Rotating orange needle
 *   - Light-gray pivot hub
 *   - Digital temperature readout (below the baked "WATER TEMP" label)
 *   - Flashing red alarm banner for CHECK SENSOR / OVERHEAT
 *
 * Face sweep geometry (must match render_pontiac_face_final.py):
 *     100 F  -> -120 deg (from 12 o'clock, cw positive)
 *     180 F ->    0 deg  (straight up)
 *     260 F -> +120 deg
 */

#include <string.h>
#include <stdio.h>

#include "lvgl.h"
#include "esp_log.h"
#include "display_gauge.h"
#include "gauge_face.h"
#include "needle_img.h"
#include "settings_screen.h"
#include "Settings.h"

static const char *TAG = "GAUGE";

/* ---------- calibration ------------------------------------------- */
/* The face artwork is rendered with the scale sweeping
 *     100 F -> -120 deg (from 12 o'clock, cw positive)
 *     180 F ->    0 deg (straight up)
 *     260 F -> +120 deg
 * for a total 240 deg sweep. Keep these in lockstep with
 * render_pontiac_face_final.py -- if the face PNG is ever re-rendered
 * with different angles the needle/arc math has to move with it. */
#define GAUGE_START_DEG   -120.0f
#define GAUGE_SWEEP_DEG    240.0f
#define GAUGE_T_MIN         100.0f
#define GAUGE_T_MAX         260.0f

/* Flip to 1 to sweep the needle end-to-end for bench testing without a
 * sender connected. With the ADS1115 + Prosport wired up, leave it 0 so
 * set_gauge_temp_f() from Temp_Sender drives the needle. */
#define GAUGE_DEMO_SWEEP    0

/* ---------- module state ------------------------------------------- */
static lv_obj_t   *s_needle        = NULL;
static lv_obj_t   *s_readout       = NULL;  /* live digital temp label   */
static lv_obj_t   *s_warn_arc      = NULL;  /* red overheat arc widget   */
static lv_obj_t   *s_fault_label   = NULL;
static lv_obj_t   *s_fault_text    = NULL;  /* child label inside banner */
static lv_timer_t *s_fault_blink   = NULL;
static bool        s_fault_on      = false;
static char        s_fault_msg[24] = {0};

/* ---------- helpers ------------------------------------------------ */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Map a temperature to our gauge-coord angle (0 = 12 o'clock, cw +). */
static float temp_to_gauge_deg(float temp_f)
{
    const float span = GAUGE_T_MAX - GAUGE_T_MIN;
    return GAUGE_START_DEG + (temp_f - GAUGE_T_MIN) * GAUGE_SWEEP_DEG / span;
}

/* Convert gauge-coord angle (0 up) to lv_arc's coordinate (0 at 3
 * o'clock, cw +). Also normalizes into [0, 360). */
static uint16_t gauge_deg_to_arc_deg(float g)
{
    float a = g + 270.0f;
    while (a < 0.0f)   a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return (uint16_t)(a + 0.5f);
}

/* ---------- public: tear down gauge widget references --------------- */
/* Called by show_settings() before it cleans the screen. Zeroing these
 * statics makes set_gauge_temp_f() / set_gauge_alarm() no-op (they guard
 * on NULL), which is what we want while the settings screen is active
 * and the gauge widgets no longer exist. The blink timer needs an
 * explicit delete because it lives in LVGL's timer list, not on the
 * screen tree that lv_obj_clean walks. */
void gauge_release(void)
{
    s_needle       = NULL;
    s_readout      = NULL;
    s_warn_arc     = NULL;
    s_fault_label  = NULL;
    s_fault_text   = NULL;
    s_fault_on     = false;
    s_fault_msg[0] = '\0';
    if (s_fault_blink) {
        lv_timer_del(s_fault_blink);
        s_fault_blink = NULL;
    }
}

/* ---------- public: needle + digital readout ----------------------- */
void set_gauge_temp_f(float temp_f)
{
    /* Screen not built yet? Silently drop -- Temp_Sender pushes every
     * 100 ms regardless of which screen is up. */
    if (s_needle == NULL) return;

    /* Clamp to the dial range before converting to angle so the needle
     * pins at the stop rather than wrapping off the dial. */
    float clamped = clampf(temp_f, GAUGE_T_MIN, GAUGE_T_MAX);
    float angle_deg = temp_to_gauge_deg(clamped);

    /* lv_img_set_angle takes cw degrees * 10 (0.1 deg units). The
     * needle image is drawn pointing UP, so angle 0 == 12 o'clock. */
    lv_img_set_angle(s_needle, (int16_t)(angle_deg * 10.0f));

    /* Digital readout uses the UNCLAMPED temperature so out-of-range
     * values (e.g. a cold 70F bench dunk) still show a real number
     * while the needle pins at 100. */
    if (s_readout) {
        char buf[8];
        /* Plain ASCII -- no degree symbol. The default Montserrat 48
         * glyph set doesn't include U+00B0 so printing it would render
         * as a blank or missing-glyph box. */
        snprintf(buf, sizeof(buf), "%.0f", temp_f);
        lv_label_set_text(s_readout, buf);
    }
}

/* ---------- public: fault overlay ---------------------------------- */
static void fault_blink_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_fault_label) return;
    static bool on = true;
    on = !on;
    /* Toggle the whole banner's opacity -- cascades to the text too. */
    lv_obj_set_style_opa(s_fault_label,
                         on ? LV_OPA_COVER : LV_OPA_30, 0);
}

void set_gauge_alarm(bool active, const char *message)
{
    if (s_fault_label == NULL) return;

    /* Keep the banner text fresh even if only the message changed. */
    if (active && message != NULL && s_fault_text != NULL) {
        if (strncmp(s_fault_msg, message, sizeof(s_fault_msg)) != 0) {
            strncpy(s_fault_msg, message, sizeof(s_fault_msg) - 1);
            s_fault_msg[sizeof(s_fault_msg) - 1] = '\0';
            lv_label_set_text(s_fault_text, s_fault_msg);
        }
    }

    if (active == s_fault_on) return;
    s_fault_on = active;

    if (active) {
        lv_obj_clear_flag(s_fault_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_fault_label, LV_OPA_COVER, 0);
        if (s_fault_blink == NULL) {
            s_fault_blink = lv_timer_create(fault_blink_cb, 250, NULL);
        }
    } else {
        lv_obj_add_flag(s_fault_label, LV_OBJ_FLAG_HIDDEN);
        if (s_fault_blink) {
            lv_timer_del(s_fault_blink);
            s_fault_blink = NULL;
        }
    }
}

/* ---------- demo sweep --------------------------------------------- */
#if GAUGE_DEMO_SWEEP
static void sweep_cb(lv_timer_t *t)
{
    (void)t;
    static float temp  = GAUGE_T_MIN;
    static float step  = 2.0f;

    temp += step;
    if (temp >= GAUGE_T_MAX) { temp = GAUGE_T_MAX; step = -step; }
    if (temp <= GAUGE_T_MIN) { temp = GAUGE_T_MIN; step = -step; }

    set_gauge_temp_f(temp);
}
#endif

/* ---------- long-press -> settings screen -------------------------- */
static void gauge_long_press_cb(lv_event_t *e)
{
    (void)e;
    show_settings();
}

/* ---------- build the screen --------------------------------------- */
void show_gauge(void)
{
    ESP_LOGI(TAG, "show_gauge: single Pontiac face, 100..260 F");

    /* FRESH SCREEN PATTERN: create a brand-new screen, build the gauge
     * on it, then swap. Old screen gets deleted by lv_scr_load via
     * auto_del=true. This avoids the state-leak bugs we hit when we
     * tried lv_obj_clean(lv_scr_act()) in place. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 480);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLL_CHAIN);

    /* Invisible full-screen touch catcher. Bottom of the z-stack so
     * face/needle/readout/cap draw on top of it. Long-press anywhere
     * on the dial opens the settings screen. */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, 480, 480);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, gauge_long_press_cb,
                        LV_EVENT_LONG_PRESSED, NULL);

    /* Face: full-screen static Pontiac-themed artwork. */
    lv_obj_t *face = lv_img_create(scr);
    lv_img_set_src(face, &gauge_face_classic);
    lv_obj_set_pos(face, 0, 0);

    /* Live red overheat arc. Background arc is the colored band; the
     * indicator arc is hidden. When the user's trip is below 240 F the
     * band extends past the baked red zone; when it's at 240 the live
     * arc and the baked art overlap and look continuous. */
    s_warn_arc = lv_arc_create(scr);
    lv_obj_set_size(s_warn_arc, 440, 440);
    lv_obj_set_pos(s_warn_arc, 20, 20);
    lv_obj_remove_style(s_warn_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_warn_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_warn_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_warn_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_warn_arc, 0, 0);
    lv_obj_set_style_arc_color(s_warn_arc, lv_color_hex(0xFF1919),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_warn_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_warn_arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_warn_arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_warn_arc, LV_OPA_TRANSP, LV_PART_INDICATOR);

    /* Clamp trip into the dial range, then plot start..end angles. */
    float trip_f = Settings_GetOverheatTripF();
    if (trip_f < GAUGE_T_MIN) trip_f = GAUGE_T_MIN;
    if (trip_f > GAUGE_T_MAX) trip_f = GAUGE_T_MAX;
    uint16_t a0 = gauge_deg_to_arc_deg(temp_to_gauge_deg(trip_f));
    uint16_t a1 = gauge_deg_to_arc_deg(temp_to_gauge_deg(GAUGE_T_MAX));
    lv_arc_set_bg_angles(s_warn_arc, a0, a1);
    lv_arc_set_angles(s_warn_arc, a0, a0);

    /* Live digital readout -- sits BELOW the baked "WATER TEMP" label.
     * Face's WATER TEMP text (16 px DejaVu Sans Bold) starts at y=322
     * and bottoms out around y=338. A Montserrat 48 readout centered
     * at CY+135 (y=375) spans roughly y=351..399, leaving ~13 px of
     * clear space under WATER TEMP so the two labels don't touch.
     *
     * Font choice: the project's Kconfig.projbuild only exposes 12/14/16
     * but LVGL's own Kconfig ships with _28 and _48 also enabled in this
     * project's sdkconfig, so the symbols are in the binary. There's no
     * 32/36/40 compiled in -- it's 28 or 48 for a "big" readout, and
     * 28 reads as too small on this face. Use 48. */
    s_readout = lv_label_create(scr);
    lv_label_set_text(s_readout, "---");
    lv_obj_set_style_text_color(s_readout, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_readout, &lv_font_montserrat_48, 0);
    /* Fixed 120 px wide container with center text alignment.  This pins
     * the readout's right edge regardless of whether the text is "---",
     * "100", or "260", so the F unit anchored to OUT_RIGHT_BOTTOM stays
     * clear of the digits at all values. */
    lv_obj_set_width(s_readout, 120);
    lv_obj_set_style_text_align(s_readout, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_readout, LV_ALIGN_CENTER, 0, 135);

    /* "F" unit tag to the right of the readout, one size down so it
     * reads as a unit suffix rather than another digit. */
    lv_obj_t *unit = lv_label_create(scr);
    lv_label_set_text(unit, "F");
    lv_obj_set_style_text_color(unit, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_28, 0);
    lv_obj_align_to(unit, s_readout, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);

    /* Needle: 80x480 image with pivot at (40, 240). Placed so the pivot
     * sits at screen (240, 240). */
    s_needle = lv_img_create(scr);
    lv_img_set_src(s_needle, &needle_img);
    lv_obj_set_pos(s_needle, 200, 0);
    lv_img_set_pivot(s_needle, 40, 240);
    /* Paint at the center of the range so the power-on pose is sensible. */
    set_gauge_temp_f((GAUGE_T_MIN + GAUGE_T_MAX) * 0.5f);

    /* Pivot cap overlay -- sits ON TOP of the needle so the needle's
     * root is always hidden behind it. Flat light gray with a subtle
     * vertical gradient to hint at a domed metal cap. No white
     * specular highlight, no two-tone rim. */
    lv_obj_t *cap = lv_obj_create(scr);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 28, 28);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(0xD2D2D4), 0);
    lv_obj_set_style_bg_grad_color(cap, lv_color_hex(0x78787C), 0);
    lv_obj_set_style_bg_grad_dir(cap, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);

    /* Alarm overlay -- bright red banner that flashes for two cases:
     *   "CHECK SENSOR" -- sender disconnected or shorted
     *   "OVERHEAT"     -- coolant >= the trip value in Settings.
     * Positioned BELOW the digital readout so blink-opacity toggling
     * doesn't chop up the readout. y=+155 from center lands it in the
     * clear area between the readout and the round display's edge. */
    s_fault_label = lv_obj_create(scr);
    lv_obj_remove_style_all(s_fault_label);
    lv_obj_set_size(s_fault_label, 220, 40);
    /* Fault banner sits below the digital readout. Readout bottom is
     * at y ~399 (CY+135, 48 px tall); banner center at CY+185 (y=425)
     * with 40 px height spans y=405..445, leaving a 6 px gap below the
     * readout. The round panel still clears the banner bounds at that
     * row (banner is 220 wide, circle allows ~266 wide at y=425). */
    lv_obj_align(s_fault_label, LV_ALIGN_CENTER, 0, 185);
    lv_obj_set_style_radius(s_fault_label, 8, 0);
    lv_obj_set_style_bg_color(s_fault_label, lv_color_hex(0xD81020), 0);
    lv_obj_set_style_bg_opa(s_fault_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_fault_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_fault_label, 2, 0);
    lv_obj_clear_flag(s_fault_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_fault_label, LV_OBJ_FLAG_CLICKABLE);

    s_fault_text = lv_label_create(s_fault_label);
    lv_label_set_text(s_fault_text, "");
    lv_obj_set_style_text_color(s_fault_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_fault_text);

    lv_obj_add_flag(s_fault_label, LV_OBJ_FLAG_HIDDEN);
    s_fault_on = false;
    s_fault_msg[0] = '\0';

#if GAUGE_DEMO_SWEEP
    lv_timer_create(sweep_cb, 40, NULL);
#endif

    /* Activate the freshly-built screen. auto_del=true frees the
     * previous screen so there's no leak across toggles. */
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}
