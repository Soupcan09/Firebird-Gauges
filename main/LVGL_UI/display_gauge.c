/*
 * display_gauge.c
 *
 * Firebird water temp gauge screen: static face image + rotatable needle
 * overlay. Also includes a demo sweep timer that moves the needle from
 * 100 F to 260 F and back every few seconds so you can verify rotation
 * and angle calibration before we wire up the real sensor.
 *
 * To wire the real GM sender later: remove/disable the sweep timer and
 * call set_gauge_temp_f() from your ADC read loop instead.
 */

#include <string.h>

#include "lvgl.h"
#include "display_gauge.h"
#include "gauge_face.h"
#include "needle_img.h"

/* Calibration: matches the artwork.
 *   100 F -> -110 deg (from 12 o'clock, cw positive)
 *   180 F ->    0 deg (straight up)
 *   260 F -> +110 deg
 */
#define GAUGE_TEMP_MIN_F   100.0f
#define GAUGE_TEMP_MAX_F   260.0f
#define GAUGE_START_DEG   -110.0f
#define GAUGE_SWEEP_DEG    220.0f

/* Flip this to 1 to sweep the needle between 100-260F for bench testing
 * without a sender connected. With the ADS1115 + TS6 wired up, leave it 0
 * so set_gauge_temp_f() from Temp_Sender drives the needle. */
#define GAUGE_DEMO_SWEEP    0

static lv_obj_t   *s_needle       = NULL;
static lv_obj_t   *s_fault_label  = NULL;
static lv_obj_t   *s_fault_text   = NULL;   /* child label inside banner */
static lv_timer_t *s_fault_blink  = NULL;
static bool        s_fault_on     = false;
static char        s_fault_msg[24] = {0};   /* last message shown */

/* --- Public: set the needle to a temperature (F). ------------------ */
void set_gauge_temp_f(float temp_f)
{
    if (s_needle == NULL) return;

    if (temp_f < GAUGE_TEMP_MIN_F) temp_f = GAUGE_TEMP_MIN_F;
    if (temp_f > GAUGE_TEMP_MAX_F) temp_f = GAUGE_TEMP_MAX_F;

    float angle_deg = GAUGE_START_DEG +
        (temp_f - GAUGE_TEMP_MIN_F) * GAUGE_SWEEP_DEG /
        (GAUGE_TEMP_MAX_F - GAUGE_TEMP_MIN_F);

    /* lv_img_set_angle takes cw degrees * 10 (0.1 deg units). Our
     * needle image is drawn pointing UP, so angle 0 == 12 o'clock. */
    int16_t ang = (int16_t)(angle_deg * 10.0f);
    lv_img_set_angle(s_needle, ang);
}

/* --- Public: fault overlay ----------------------------------------- */
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

    if (active == s_fault_on) return;    /* visibility unchanged */
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

/* --- Demo sweep timer callback ------------------------------------- */
#if GAUGE_DEMO_SWEEP
static void sweep_cb(lv_timer_t *t)
{
    static float temp  = GAUGE_TEMP_MIN_F;
    static float step  = 2.0f;   /* degrees F per tick */

    temp += step;
    if (temp >= GAUGE_TEMP_MAX_F) { temp = GAUGE_TEMP_MAX_F; step = -step; }
    if (temp <= GAUGE_TEMP_MIN_F) { temp = GAUGE_TEMP_MIN_F; step = -step; }

    set_gauge_temp_f(temp);
}
#endif

void show_gauge(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Face: full-screen static artwork */
    lv_obj_t *face = lv_img_create(scr);
    lv_img_set_src(face, &gauge_face);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, 0);

    /* Needle: 80x480 image with pivot at (40, 240).
     * Placed so the pivot sits at screen (240, 240). */
    s_needle = lv_img_create(scr);
    lv_img_set_src(s_needle, &needle_img);
    lv_obj_set_pos(s_needle, 200, 0);
    lv_img_set_pivot(s_needle, 40, 240);
    /* Start at 180 F (straight up) */
    set_gauge_temp_f(180.0f);

    /* Pivot cap overlay -- sits ON TOP of the needle so the needle's
     * root is hidden behind it. Shiny silver look: bright top fading
     * to near-black bottom, with a small white specular highlight,
     * matching the radial-gradient cap drawn into the face artwork. */
    lv_obj_t *cap = lv_obj_create(scr);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 28, 28);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(0xE0E0E0), 0);      /* top */
    lv_obj_set_style_bg_grad_color(cap, lv_color_hex(0x1A1A1A), 0); /* bottom */
    lv_obj_set_style_bg_grad_dir(cap, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);

    /* Specular highlight dot on the upper-left of the cap */
    lv_obj_t *cap_hi = lv_obj_create(scr);
    lv_obj_remove_style_all(cap_hi);
    lv_obj_set_size(cap_hi, 6, 6);
    lv_obj_align(cap_hi, LV_ALIGN_CENTER, -4, -4);
    lv_obj_set_style_radius(cap_hi, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap_hi, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(cap_hi, LV_OPA_80, 0);
    lv_obj_clear_flag(cap_hi, LV_OBJ_FLAG_SCROLLABLE);

    /* Alarm overlay -- bright red banner that flashes for two cases:
     *   "CHECK SENSOR" -- sender disconnected or shorted
     *   "OVERHEAT"     -- coolant >= 212 F
     * Hidden by default; brought up by set_gauge_alarm(true, msg) from
     * the Temp_Sender task. Uses default Montserrat 14 (only size
     * enabled in this LVGL build) but the red banner makes it very
     * visible. */
    s_fault_label = lv_obj_create(scr);
    lv_obj_remove_style_all(s_fault_label);
    lv_obj_set_size(s_fault_label, 220, 40);
    lv_obj_align(s_fault_label, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_radius(s_fault_label, 8, 0);
    lv_obj_set_style_bg_color(s_fault_label, lv_color_hex(0xD81020), 0);
    lv_obj_set_style_bg_opa(s_fault_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_fault_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_fault_label, 2, 0);
    lv_obj_clear_flag(s_fault_label, LV_OBJ_FLAG_SCROLLABLE);

    s_fault_text = lv_label_create(s_fault_label);
    lv_label_set_text(s_fault_text, "");
    lv_obj_set_style_text_color(s_fault_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(s_fault_text);

    lv_obj_add_flag(s_fault_label, LV_OBJ_FLAG_HIDDEN);
    s_fault_on = false;
    s_fault_msg[0] = '\0';

#if GAUGE_DEMO_SWEEP
    /* Tick every 40 ms -> full 100->260 sweep ~= 3.2 seconds each direction */
    lv_timer_create(sweep_cb, 40, NULL);
#endif
}
