/*
 * settings_screen.c -- one-page settings UI for the Firebird gauge.
 *
 * Visual style matches the gauge: black background, warm-orange
 * accents (0xFFB84A -- the muscle-car tach orange), rounded dark
 * cards per setting row.
 *
 * 2026-04-24: typography bumped across the board for legibility (Jeff
 * is 65 and reads the gauge from across the dashboard).  Title/values
 * are 24pt Montserrat (was 16); labels are 18pt (was 14); +/- and the
 * buzzer pill are 24pt; RESET/BACK action buttons are 20pt.
 *
 * Layout (480x480 round display):
 *
 *   +--------------------------------------------+
 *   |               SETTINGS  (24pt)             |
 *   |          72 F   9800 Ohm  (18pt live)      |
 *   |   +------------------------------------+   |
 *   |   | OVERHEAT       [-]  205 F  [+]     |   |
 *   |   +------------------------------------+   |
 *   |   | TEMP OFFSET    [-]   +0 F  [+]     |   |
 *   |   +------------------------------------+   |
 *   |   | BRIGHTNESS     [-]   70 %  [+]     |   |
 *   |   +------------------------------------+   |
 *   |   | SPLASH TIME    [-]    3 s  [+]     |   |
 *   |   +------------------------------------+   |
 *   |   | BUZZER             [ OFF / ON ]    |   |
 *   |   +------------------------------------+   |
 *   |       [ RESET ]      [ BACK ]              |
 *   +--------------------------------------------+
 *
 * The numeric +/- handlers all follow the same pattern: grab current
 * value, bump by STEP, call Settings_Set...(), repaint the label.
 * No save button -- every tap writes through to NVS.
 */

#include "lvgl.h"
#include "esp_log.h"
#include "settings_screen.h"
#include "display_gauge.h"
#include "Settings.h"
#include "Temp_Sender.h"

#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "SETTINGS";

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */
#define C_BG              0x000000   /* page background                  */
#define C_TITLE           0xFFFFFF   /* "SETTINGS"                       */
#define C_LIVE            0xFFB84A   /* live temp/ohm readout            */
#define C_CARD            0x181818   /* per-row card fill                */
#define C_CARD_BORDER     0x2E2E2E   /* subtle card outline              */
#define C_LABEL           0xB8B8B8   /* row label                        */
#define C_VALUE           0xFFB84A   /* row value text                   */
#define C_BTN_BG          0x2A2A2A   /* +/- and toggle idle              */
#define C_BTN_BG_PRESS    0x4A4A4A   /* +/- pressed                      */
#define C_BTN_TEXT        0xFFFFFF
#define C_RESET_BG        0x5A1F22   /* muted brick red                  */
#define C_RESET_BG_PRESS  0x7A2A2E
#define C_BACK_BG         0x1F5A32   /* forest green                     */
#define C_BACK_BG_PRESS   0x2A7A43
#define C_BUZZ_ON         0x1F5A32   /* buzzer enabled state             */
#define C_BUZZ_OFF        0x444444   /* buzzer disabled state            */

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static lv_obj_t   *s_trip_val    = NULL;
static lv_obj_t   *s_offset_val  = NULL;
static lv_obj_t   *s_bright_val  = NULL;
static lv_obj_t   *s_splash_val  = NULL;
static lv_obj_t   *s_buzzer_btn  = NULL;
static lv_obj_t   *s_buzzer_lbl  = NULL;
static lv_obj_t   *s_live_lbl    = NULL;
static lv_timer_t *s_live_timer  = NULL;

/* ------------------------------------------------------------------ */
/* Label refreshers                                                    */
/* ------------------------------------------------------------------ */
/* Helper: force a full-screen redraw. The RGB panel's partial-flush
 * path writes rectangles to the framebuffer with a horizontal offset
 * whenever a touch is active, so every settings click has to invalidate
 * the whole screen to push LVGL through the non-buggy full-frame flush
 * path. */
static void force_full_redraw(void)
{
    lv_obj_invalidate(lv_scr_act());
}

static void refresh_trip(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f F", Settings_GetOverheatTripF());
    lv_label_set_text(s_trip_val, buf);
    force_full_redraw();
}

static void refresh_offset(void)
{
    char buf[16];
    /* "%+.0f" gives +3 / -3 / +0 so the sign is always visible, which
     * matters here -- offset is a signed correction. */
    snprintf(buf, sizeof(buf), "%+.0f F", Settings_GetTempOffsetF());
    lv_label_set_text(s_offset_val, buf);
    force_full_redraw();
}

static void refresh_bright(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u %%", (unsigned)Settings_GetBrightness());
    lv_label_set_text(s_bright_val, buf);
    force_full_redraw();
}

static void refresh_splash(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u s", (unsigned)Settings_GetSplashTimeS());
    lv_label_set_text(s_splash_val, buf);
    force_full_redraw();
}

static void refresh_buzzer(void)
{
    bool on = Settings_GetBuzzerEnabled();
    lv_label_set_text(s_buzzer_lbl, on ? "ON" : "OFF");
    lv_obj_set_style_bg_color(s_buzzer_btn,
        lv_color_hex(on ? C_BUZZ_ON : C_BUZZ_OFF), 0);
    force_full_redraw();
}

static void refresh_all(void)
{
    refresh_trip();
    refresh_offset();
    refresh_bright();
    refresh_splash();
    refresh_buzzer();
}

/* ------------------------------------------------------------------ */
/* Event callbacks: +/- buttons                                        */
/* ------------------------------------------------------------------ */
static void trip_minus_cb(lv_event_t *e)
{
    (void)e;
    Settings_SetOverheatTripF(Settings_GetOverheatTripF() - SETTINGS_TRIP_F_STEP);
    refresh_trip();
}
static void trip_plus_cb(lv_event_t *e)
{
    (void)e;
    Settings_SetOverheatTripF(Settings_GetOverheatTripF() + SETTINGS_TRIP_F_STEP);
    refresh_trip();
}

static void offset_minus_cb(lv_event_t *e)
{
    (void)e;
    Settings_SetTempOffsetF(Settings_GetTempOffsetF() - SETTINGS_OFFSET_F_STEP);
    refresh_offset();
}
static void offset_plus_cb(lv_event_t *e)
{
    (void)e;
    Settings_SetTempOffsetF(Settings_GetTempOffsetF() + SETTINGS_OFFSET_F_STEP);
    refresh_offset();
}

static void bright_minus_cb(lv_event_t *e)
{
    (void)e;
    int cur = Settings_GetBrightness();
    Settings_SetBrightness((uint8_t)(cur - SETTINGS_BRIGHT_STEP));
    refresh_bright();
}
static void bright_plus_cb(lv_event_t *e)
{
    (void)e;
    int cur = Settings_GetBrightness();
    Settings_SetBrightness((uint8_t)(cur + SETTINGS_BRIGHT_STEP));
    refresh_bright();
}

static void splash_minus_cb(lv_event_t *e)
{
    (void)e;
    int cur = Settings_GetSplashTimeS();
    Settings_SetSplashTimeS((uint8_t)(cur - SETTINGS_SPLASH_S_STEP));
    refresh_splash();
}
static void splash_plus_cb(lv_event_t *e)
{
    (void)e;
    int cur = Settings_GetSplashTimeS();
    Settings_SetSplashTimeS((uint8_t)(cur + SETTINGS_SPLASH_S_STEP));
    refresh_splash();
}

static void buzzer_toggle_cb(lv_event_t *e)
{
    (void)e;
    Settings_SetBuzzerEnabled(!Settings_GetBuzzerEnabled());
    refresh_buzzer();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    Settings_ResetDefaults();
    refresh_all();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    /* Kill the live-readout timer before the screen gets cleaned;
     * otherwise it'll fire once more against a deleted label. */
    if (s_live_timer) {
        lv_timer_del(s_live_timer);
        s_live_timer = NULL;
    }
    s_trip_val = s_offset_val = s_bright_val = s_splash_val = NULL;
    s_buzzer_btn = s_buzzer_lbl = s_live_lbl = NULL;
    ESP_LOGI(TAG, "back_cb: -> show_gauge");
    show_gauge();
}

/* ------------------------------------------------------------------ */
/* Live temp/resistance readout                                        */
/* ------------------------------------------------------------------ */
static void live_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_live_lbl) return;

    float tF = TempSender_GetTempF();
    float r  = TempSender_GetResistanceOhms();

    char buf[48];
    /* Clamp to reasonable display ranges so weird fault values don't
     * blow the layout width. Resistance can hit 1e9 on an open sender. */
    if (r > 99999.0f) r = 99999.0f;
    if (r < 1.0f)     r = 1.0f;
    /* "Ohm" instead of the greek Omega symbol -- Omega isn't always
     * in the Montserrat font LVGL ships; "Ohm" renders reliably. */
    snprintf(buf, sizeof(buf), "%.0f F     %.0f Ohm", tF, r);
    lv_label_set_text(s_live_lbl, buf);
}

/* ------------------------------------------------------------------ */
/* Widget factories                                                    */
/* ------------------------------------------------------------------ */

/* Kill every scroll behavior on a button/card. lv_btn_create inherits
 * LV_OBJ_FLAG_SCROLLABLE from lv_obj, and LV_OBJ_FLAG_SCROLL_ELASTIC is
 * default-on in the theme. On the CST820 touch panel every tap has a
 * few pixels of micro-drift before the release event, which LVGL
 * interprets as a drag -- and because the widget is "scrollable" with
 * zero scrollable content, the elastic-bounce code translates the
 * widget visually along the drag axis. That's the "screen jumps right
 * when I touch it" bug. Clear every scroll-related flag on every
 * touchable widget so LVGL never engages its drag/scroll machinery. */
static void kill_scroll(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

/* Pressed/released event hook -- forces a full-screen invalidate on
 * every touch state transition so LVGL never ships a small partial-rect
 * flush to the RGB panel mid-touch. This panel's partial-flush path
 * renders a horizontal offset whenever the flush region is smaller than
 * the full frame during an active touch; the user-visible symptom is
 * "screen jumps right when I press a button". Forcing a full invalidate
 * keeps LVGL on the non-buggy full-frame flush path for every press and
 * release. Heavy-handed (we redraw the whole 480x480 for a button color
 * change) but it's the only reliable workaround until the panel driver
 * is reworked. */
static void btn_flush_on_press_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_invalidate(lv_scr_act());
}

/* Plain clickable rectangle -- NOT an lv_btn. We use this instead of
 * lv_btn_create because the LVGL 8.2 default theme attaches a
 * translate_y (and transform_width) transition to LV_STATE_PRESSED on
 * every lv_btn. On this RGB panel with its vsync semaphore config, that
 * press-state transform is rendering as a full-screen horizontal shift
 * every single click. Building buttons from a raw lv_obj with
 * remove_style_all strips all theme state-transition styles at the
 * source so there is nothing for the panel's sync behavior to stutter
 * on. Click semantics come from LV_OBJ_FLAG_CLICKABLE; we supply our
 * own bg_color defaults and pressed-state bg_color.
 *
 * We ALSO attach btn_flush_on_press_cb to every such widget so the
 * PRESSED/RELEASED frames go through a full-screen invalidate, not the
 * tiny button-rect invalidate that triggers the RGB panel's partial-
 * flush offset bug. Wiring the flush handler in here (rather than at
 * each caller) means every clickable widget on this screen -- step
 * buttons, buzzer toggle, RESET DEFAULTS, BACK TO GAUGE -- gets the
 * treatment automatically. */
static lv_obj_t *make_plain_clickable(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    kill_scroll(btn);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, btn_flush_on_press_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(btn, btn_flush_on_press_cb, LV_EVENT_RELEASED, NULL);
    return btn;
}

/* Circular (square with big radius) +/- style button, 48x48. Returns
 * the clickable obj so the caller can position it. Same size as before
 * but with 24pt glyphs (was 16pt) -- Jeff is 65, the +/- need to read
 * at a glance.  Held to 48 so it fits inside the 56-tall card with
 * 4 px top/bottom padding. */
static lv_obj_t *make_step_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = make_plain_clickable(parent);
    lv_obj_set_size(btn, 48, 48);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN_BG), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN_BG_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(C_BTN_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    return btn;
}

/* One numeric setting row: card with [label] .... [-] [value] [+].
 * Card 360 wide x 60 tall (was 340 x 52) so 24pt values + 52x52 step
 * buttons fit comfortably. Returns the value label so the caller can
 * store it for later refresh_*() calls. */
static lv_obj_t *make_num_row(lv_obj_t *parent,
                              const char *label_text,
                              lv_event_cb_t minus_cb,
                              lv_event_cb_t plus_cb)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    kill_scroll(card);
    lv_obj_set_size(card, 360, 56);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_CARD_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);

    /* Label on the left -- 18pt for legibility (was 14) */
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_LABEL), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

    /* [+] flush right, [-] left of value. Value centered in its gap.
     * Step buttons are 48x48; value column is 92 wide. Card is 360
     * wide so + button right-edge at 360-10 = 350, value column from
     * (260, 350-48-92) to 260, - button left of that. */
    lv_obj_t *plus = make_step_btn(card, "+", plus_cb);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -10, 0);

    lv_obj_t *minus = make_step_btn(card, "-", minus_cb);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -10 - 48 - 92, 0);

    lv_obj_t *val = lv_label_create(card);
    lv_label_set_text(val, "");
    lv_obj_set_style_text_color(val, lv_color_hex(C_VALUE), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
    lv_obj_set_width(val, 88);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10 - 48 - 2, 0);

    return val;
}

/* Buzzer row: label + single toggle pill showing OFF / ON. Same 360x60
 * card size and font scale as the numeric rows. */
static void make_buzzer_row(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    kill_scroll(card);
    lv_obj_set_size(card, 360, 56);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_CARD_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, "BUZZER");
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_LABEL), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

    s_buzzer_btn = make_plain_clickable(card);
    lv_obj_set_size(s_buzzer_btn, 130, 42);
    lv_obj_set_style_radius(s_buzzer_btn, 21, 0);
    lv_obj_set_style_bg_color(s_buzzer_btn, lv_color_hex(C_BUZZ_OFF), 0);
    lv_obj_set_style_border_width(s_buzzer_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_buzzer_btn, 0, 0);
    lv_obj_align(s_buzzer_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(s_buzzer_btn, buzzer_toggle_cb, LV_EVENT_CLICKED, NULL);

    s_buzzer_lbl = lv_label_create(s_buzzer_btn);
    lv_label_set_text(s_buzzer_lbl, "OFF");
    lv_obj_set_style_text_color(s_buzzer_lbl, lv_color_hex(C_BTN_TEXT), 0);
    lv_obj_set_style_text_font(s_buzzer_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_buzzer_lbl);
}

/* Action button (RESET / BACK).  Caller sets size + position; this
 * helper just supplies the look + click handler.  20pt label (was 16)
 * for legibility. */
static lv_obj_t *make_action_btn(lv_obj_t *parent,
                                 const char *text,
                                 uint32_t bg_hex,
                                 uint32_t bg_press_hex,
                                 lv_event_cb_t cb)
{
    lv_obj_t *btn = make_plain_clickable(parent);
    lv_obj_set_size(btn, 300, 42);
    lv_obj_set_style_radius(btn, 21, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_press_hex), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(C_BTN_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return btn;
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */
void show_settings(void)
{
    /* Defensive: if a previous session left the timer running, kill it. */
    if (s_live_timer) {
        lv_timer_del(s_live_timer);
        s_live_timer = NULL;
    }

    /* Tell the gauge module its widgets are about to go away so the
     * Temp_Sender task's ongoing set_gauge_temp_f / set_gauge_alarm
     * calls no-op during our lifetime. */
    gauge_release();

    /* FRESH SCREEN PATTERN (matches display_gauge.c). Every previous
     * attempt to fix "screen jumps right on touch" tried to scrub the
     * CURRENT screen (lv_obj_clean + remove_style_all + flag kills on
     * lv_scr_act()). It never worked because lv_obj_clean only touches
     * CHILDREN -- the screen object itself kept its scroll offset, its
     * flags, and its theme style residue across every show_*() call.
     * That's state leak: settings-screen patches bled back into the
     * gauge screen, shifting it too.
     *
     * Build a brand-new screen instead, then lv_scr_load it with
     * auto_del so the old one is freed for us. Guaranteed virgin state
     * every time the user opens Settings.
     *
     * Layout on 480x480 round panel (visible area = circle r=240 at
     * center 240,240). Cards are 340 wide so CARD_X = (480-340)/2 = 70
     * centers them. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 480);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
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

    /* Card geometry ----------------------------------------------- */
    /* Cards are 360 wide (helpers hardcode internal +/- offsets
     * relative to that). CARD_X = (480-360)/2 = 60 centers them.
     *
     * 5 cards now (added SPLASH TIME).  Action buttons placed
     * side-by-side at the bottom to claw back vertical space.
     * Cards 56 tall, stride 60 (4 px gap).  Layout:
     *   y=  18  Title (24pt)
     *   y=  50  Live readout (18pt)
     *   y=  78  OVERHEAT card
     *   y= 138  TEMP OFFSET card
     *   y= 198  BRIGHTNESS card
     *   y= 258  SPLASH TIME card
     *   y= 318  BUZZER card
     *   y= 384  RESET / BACK side-by-side, h=42, ends at y=426
     * y=426 is well inside the 480-circle (visible chord ~ 308 px).
     */
    const int CARD_X       =  60;
    const int CARD_Y0      =  78;
    const int CARD_STRIDE  =  60;
    const int ACT_Y        = 384;
    const int ACT_H        =  42;
    const int ACT_W        = 138;
    const int ACT_GAP      =  16;
    const int ACT_X_LEFT   = (480 - (2 * ACT_W + ACT_GAP)) / 2;        /* 94  */
    const int ACT_X_RIGHT  = ACT_X_LEFT + ACT_W + ACT_GAP;             /* 248 */

    /* Title ------------------------------------------------------- */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    /* Live readout ------------------------------------------------ */
    s_live_lbl = lv_label_create(scr);
    lv_label_set_text(s_live_lbl, "---");
    lv_obj_set_style_text_color(s_live_lbl, lv_color_hex(C_LIVE), 0);
    lv_obj_set_style_text_font(s_live_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(s_live_lbl, LV_ALIGN_TOP_MID, 0, 50);

    /* Card rows pinned absolutely. */
    s_trip_val   = make_num_row(scr, "OVERHEAT",    trip_minus_cb,   trip_plus_cb);
    lv_obj_set_pos(lv_obj_get_parent(s_trip_val),   CARD_X, CARD_Y0 + 0 * CARD_STRIDE);

    s_offset_val = make_num_row(scr, "TEMP OFFSET", offset_minus_cb, offset_plus_cb);
    lv_obj_set_pos(lv_obj_get_parent(s_offset_val), CARD_X, CARD_Y0 + 1 * CARD_STRIDE);

    s_bright_val = make_num_row(scr, "BRIGHTNESS",  bright_minus_cb, bright_plus_cb);
    lv_obj_set_pos(lv_obj_get_parent(s_bright_val), CARD_X, CARD_Y0 + 2 * CARD_STRIDE);

    s_splash_val = make_num_row(scr, "SPLASH TIME", splash_minus_cb, splash_plus_cb);
    lv_obj_set_pos(lv_obj_get_parent(s_splash_val), CARD_X, CARD_Y0 + 3 * CARD_STRIDE);

    make_buzzer_row(scr);
    lv_obj_set_pos(lv_obj_get_parent(s_buzzer_btn), CARD_X, CARD_Y0 + 4 * CARD_STRIDE);

    /* Action buttons (side-by-side) ------------------------------ */
    lv_obj_t *rst  = make_action_btn(scr, "RESET",
                                     C_RESET_BG, C_RESET_BG_PRESS, reset_cb);
    lv_obj_set_size(rst,  ACT_W, ACT_H);
    lv_obj_set_pos(rst,   ACT_X_LEFT, ACT_Y);

    lv_obj_t *back = make_action_btn(scr, "BACK",
                                     C_BACK_BG, C_BACK_BG_PRESS, back_cb);
    lv_obj_set_size(back, ACT_W, ACT_H);
    lv_obj_set_pos(back,  ACT_X_RIGHT, ACT_Y);

    /* Populate all values --------------------------------------- */
    refresh_all();
    live_tick(NULL);
    s_live_timer = lv_timer_create(live_tick, 200, NULL);

    /* Swap to the freshly-built settings screen. auto_del=true frees
     * the previous gauge screen, so no leak and no state carried over. */
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}
