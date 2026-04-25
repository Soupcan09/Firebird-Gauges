/*
 * Temp_Sender.c -- Prosport PSOWTS-JPN coolant temp sender via ADS1115
 * over I2C.
 *
 * Uses the board's existing I2C bus (I2C_NUM_0 at 400 kHz, set up by
 * I2C_Driver). ADS1115 default address 0x48, channel AIN0 single-ended.
 *
 * The PROSPORT_CURVE[] table is the 7-point factory spec from Prosport
 * (104/140/176/212/248/284/302 F). If you swap to a different sender
 * later, only that table needs to change. User-tunable offset in
 * Settings lets you bias the final reading without touching the curve.
 */
#include "Temp_Sender.h"

#include <math.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "I2C_Driver.h"      /* i2c bus handle / I2C_MASTER_NUM, etc. */
#include "display_gauge.h"   /* set_gauge_temp_f */
#include "Buzzer.h"          /* Buzzer_On / Buzzer_Off                */
#include "Settings.h"        /* overheat trip, temp offset, buzzer    */

/* The buzzer enable lives in Settings (NVS-backed, user-toggleable
 * from the on-screen SETTINGS page) instead of a compile-time flag.
 * This lets you bench-test silently and then arm the buzzer without
 * a re-flash when the gauge goes in the car. */

static const char *TAG = "TempSender";

/* ------------------------------------------------------------------ */
/* ADS1115 registers & config                                          */
/* ------------------------------------------------------------------ */
#define ADS1115_ADDR           0x48   /* ADDR pin tied to GND */
#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG     0x01

/* Single-shot, AIN0 vs GND, FSR = +/-4.096V (PGA gain 1),
 * 128 SPS, comparator disabled.
 *   OS=1  MUX=100(AIN0/GND)  PGA=001(+/-4.096V)  MODE=1(single)
 *   DR=100(128SPS)  COMP_MODE=0  COMP_POL=0  COMP_LAT=0  COMP_QUE=11
 *   = 0xC383
 */
#define ADS1115_CFG_SINGLE_A0  0xC383
#define ADS1115_FSR_VOLTS      4.096f  /* volts full-scale */

/* 1 kohm, 1% metal-film pullup between 3.3V and the A0 input pin. */
#define PULLUP_OHMS            1000.0f
#define VCC_VOLTS              3.3f

/* Sensor fault detection thresholds.
 *   V_ADC > V_OPEN_THRESH  => sender is open-circuit (broken wire,
 *                             corroded connector, missing sender).
 *   V_ADC < V_SHORT_THRESH => sender is hard-shorted. A genuine 260F
 *                             reading sits at ~0.17V so we leave a
 *                             little margin below that for "real hot".
 *   We require CONSEC_FAULT_SAMPLES bad reads in a row before raising
 *   the alarm, so a single noise spike won't trip it. */
#define V_OPEN_THRESH          3.20f
#define V_SHORT_THRESH         0.05f
#define CONSEC_FAULT_SAMPLES   3

/* Overheat alarm thresholds (F) now live in Settings (NVS). They are
 * read live each sample tick via Settings_GetOverheatTripF() and
 * Settings_GetOverheatClearF() so changes from the on-screen settings
 * page take effect immediately, no reboot. The clear threshold is
 * always trip - SETTINGS_HYST_F (4 F) for consistent hysteresis. */

/* ------------------------------------------------------------------ */
/* Prosport PSOWTS-JPN resistance curve                                */                                          
/* ------------------------------------------------------------------ */
typedef struct { float ohms; float temp_f; } ts_point_t;
static const ts_point_t PROSPORT_CURVE[] = {
    /* Prosport PSOWTS-JPN — factory spec, 7 points, 104-302 F */
    { 5830.0f, 104.0f },
    { 3020.0f, 140.0f },
    { 1670.0f, 176.0f },
    {  975.0f, 212.0f },   /* boiling */
    {  599.0f, 248.0f },
    {  386.0f, 284.0f },
    {  316.0f, 302.0f },
};
#define PROSPORT_POINTS (sizeof(PROSPORT_CURVE) / sizeof(PROSPORT_CURVE[0]))

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static float s_temp_f       = 180.0f;
static float s_resistance   = 210.0f;
static bool  s_initialized  = false;

/* ------------------------------------------------------------------ */
/* Low-level I2C helpers                                               */
/* ------------------------------------------------------------------ */
static esp_err_t ads_write_reg16(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_write_to_device(I2C_MASTER_NUM, ADS1115_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t ads_read_reg16(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = {0};
    esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM,
                                                 ADS1115_ADDR,
                                                 &reg, 1,
                                                 rx, 2,
                                                 pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        *out = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return err;
}

/* Trigger a single conversion on AIN0 and return the signed 16-bit
 * result. At PGA=1 (+/-4.096V), 1 LSB = 125 uV. */
static bool ads_read_a0(int16_t *out_raw)
{
    if (ads_write_reg16(ADS1115_REG_CONFIG, ADS1115_CFG_SINGLE_A0) != ESP_OK) {
        return false;
    }
    /* 128 SPS -> about 7.8 ms/conversion. Give it 10 ms. */
    vTaskDelay(pdMS_TO_TICKS(10));

    uint16_t raw = 0;
    if (ads_read_reg16(ADS1115_REG_CONVERSION, &raw) != ESP_OK) {
        return false;
    }
    *out_raw = (int16_t)raw;
    return true;
}

/* ------------------------------------------------------------------ */
/* Math                                                                */
/* ------------------------------------------------------------------ */
static float adc_raw_to_volts(int16_t raw)
{
    if (raw < 0) raw = 0;              /* negatives = noise below GND */
    return ((float)raw / 32767.0f) * ADS1115_FSR_VOLTS;
}

/* Voltage at ADC -> sender resistance via the divider math:
 *   Vadc = Vcc * Rs / (Rs + Rpull)
 *   Rs   = Rpull * Vadc / (Vcc - Vadc)
 */
static float volts_to_resistance(float vadc)
{
    if (vadc <= 0.001f)          return 1e9f;   /* open circuit */
    if (vadc >= VCC_VOLTS - 0.01f) return 0.0f;  /* short to GND */
    return PULLUP_OHMS * vadc / (VCC_VOLTS - vadc);
}

/* --- Cold-end Beta extrapolation -----------------------------------
 * Prosport's published curve stops at 104 F / 5830 ohm. Below that, a
 * flat clamp made ambient readings useless (gauge stuck at 104 F at
 * room temp). Instead, model the NTC with the standard Beta equation
 *   R(T) = R0 * exp( Beta * (1/T - 1/T0) )
 * where Beta is derived once from the two coldest factory points:
 *   5830 ohm @ 313.15 K (104 F)  and  3020 ohm @ 333.15 K (140 F)
 *   Beta = ln(5830/3020) / (1/313.15 - 1/333.15) ~= 3432 K
 * Accuracy is ~+/- a few F down to room temp, softer near freezing,
 * but a whole lot better than a pinned 104 F. We'll replace this with
 * measured ice-water / ambient points in the table later and the
 * Beta branch will stop firing. */
#define BETA_R0_OHMS       5830.0f        /* PROSPORT_CURVE[0].ohms  */
#define BETA_T0_KELVIN      313.15f       /* 40 C == 104 F           */
#define BETA_K             3432.0f        /* fit from first two pts  */

/* Piecewise-linear interpolation through PROSPORT_CURVE. Curve is in
 * descending-ohm order; scan high->low until we bracket r, then linearly
 * interpolate temp_f. Above the coldest table point we extrapolate with
 * the NTC Beta model (see above); below the hottest point we clamp. */
static float resistance_to_temp_f(float r)
{
    const size_t n = sizeof(PROSPORT_CURVE) / sizeof(PROSPORT_CURVE[0]);

    /* Cold side: extrapolate instead of clamping at 104 F. */
    if (r > PROSPORT_CURVE[0].ohms) {
        /* 1/T = 1/T0 + ln(R/R0) / Beta  */
        float inv_T = (1.0f / BETA_T0_KELVIN) + logf(r / BETA_R0_OHMS) / BETA_K;
        if (inv_T <= 0.0f) return -40.0f;        /* guard: pathological r */
        float T_K = 1.0f / inv_T;
        float T_C = T_K - 273.15f;
        return T_C * 9.0f / 5.0f + 32.0f;
    }

    /* Hot side: clamp at the top of the table. */
    if (r <= PROSPORT_CURVE[n - 1].ohms) return PROSPORT_CURVE[n - 1].temp_f;

    /* Inside the table: linear interpolate between bracketing points. */
    for (size_t i = 0; i < n - 1; i++) {
        float r_hi = PROSPORT_CURVE[i].ohms;
        float r_lo = PROSPORT_CURVE[i + 1].ohms;
        if (r <= r_hi && r >= r_lo) {
            float t_hi = PROSPORT_CURVE[i].temp_f;
            float t_lo = PROSPORT_CURVE[i + 1].temp_f;
            float frac = (r - r_lo) / (r_hi - r_lo);
            return t_lo + frac * (t_hi - t_lo);
        }
    }
    return PROSPORT_CURVE[n - 1].temp_f;  /* fallback, shouldn't hit */
}
/* ------------------------------------------------------------------ */
/* Background sampling task                                            */
/* ------------------------------------------------------------------ */
#define SAMPLE_PERIOD_MS   100      /* 10 Hz */
#define AVG_WINDOW         4        /* ~0.4 s smoothing -- snappy enough
                                       for bench dunk tests while still
                                       killing ADC tick-to-tick noise.  */

static void temp_task(void *arg)
{
    (void)arg;

    float window[AVG_WINDOW];
    for (int i = 0; i < AVG_WINDOW; ++i) window[i] = 180.0f;
    int widx = 0;

    int  fault_streak     = 0;      /* consecutive bad samples           */
    bool fault_latched    = false;  /* sender open/short alarm active?   */
    bool overheat_latched = false;  /* coolant >= Settings trip value?   */
    int  buzzer_tick      = 0;      /* for chirp timing (100 ms units)   */

    for (;;) {
        int16_t raw = 0;
        if (ads_read_a0(&raw)) {
            float v  = adc_raw_to_volts(raw);

            /* --- Fault detection -------------------------------- */
            bool bad = (v > V_OPEN_THRESH) || (v < V_SHORT_THRESH);
            if (bad) {
                if (fault_streak < CONSEC_FAULT_SAMPLES) fault_streak++;
            } else {
                fault_streak = 0;
            }

            if (fault_streak >= CONSEC_FAULT_SAMPLES && !fault_latched) {
                fault_latched = true;
                buzzer_tick   = 0;
                ESP_LOGW(TAG, "SENDER FAULT: V=%.3f (open>%.2f, short<%.2f)",
                         v, V_OPEN_THRESH, V_SHORT_THRESH);
                set_gauge_temp_f(260.0f);   /* peg needle to max */
            } else if (fault_streak == 0 && fault_latched) {
                fault_latched = false;
                ESP_LOGI(TAG, "sender recovered");
            }

            /* --- Normal reading path ---------------------------- */
            if (!fault_latched) {
                float r  = volts_to_resistance(v);
                /* User-adjustable offset: applied BEFORE the smoothing
                 * window and BEFORE the overheat check so that dialing
                 * +3F in settings shifts both the displayed number and
                 * the trip threshold by the same amount -- they can't
                 * get out of sync. */
                float tF = resistance_to_temp_f(r) + Settings_GetTempOffsetF();

                window[widx] = tF;
                widx = (widx + 1) % AVG_WINDOW;

                float sum = 0.0f;
                for (int i = 0; i < AVG_WINDOW; ++i) sum += window[i];
                s_temp_f     = sum / (float)AVG_WINDOW;
                s_resistance = r;

                set_gauge_temp_f(s_temp_f);
            }

            /* --- Overheat detection (hysteresis) ---------------- */
            /* Only meaningful when the sender reading is trustworthy.
             * If a fault is latched the needle is pegged at 260 and we
             * can't tell what the engine is actually doing -- treat it
             * as a sensor problem only and don't double-alarm.
             * Trip/clear are read every tick from Settings so the user
             * can adjust the threshold live from the on-screen UI. */
            if (!fault_latched) {
                float trip_f  = Settings_GetOverheatTripF();
                float clear_f = Settings_GetOverheatClearF();
                if (!overheat_latched && s_temp_f >= trip_f) {
                    overheat_latched = true;
                    buzzer_tick      = 0;
                    ESP_LOGW(TAG, "OVERHEAT: %.1f F (trip=%.0f)",
                             s_temp_f, trip_f);
                } else if (overheat_latched && s_temp_f <= clear_f) {
                    overheat_latched = false;
                    ESP_LOGI(TAG, "overheat cleared at %.1f F", s_temp_f);
                }
            }

            /* --- Drive the alarm banner ------------------------- */
            /* Priority: sender fault wins over overheat. If we can't
             * trust the reading, "CHECK SENSOR" is the more useful
             * message. The setter is idempotent, so re-asserting every
             * tick is cheap and also fixes the splash->gauge race
             * where the banner would be missed if the fault tripped
             * before show_gauge() created the widgets. */
            if (fault_latched) {
                set_gauge_alarm(true, "CHECK SENSOR");
            } else if (overheat_latched) {
                set_gauge_alarm(true, "OVERHEAT");
            } else {
                set_gauge_alarm(false, NULL);
            }

            /* Chirp pattern while any alarm is active AND the user has
             * the buzzer enabled in Settings: 150 ms ON, 850 ms OFF,
             * repeating every 1 s. At 100 ms/sample that's tick 0-1 on,
             * tick 2-9 off. Annoying enough to notice, not so bad
             * you'll unplug the screen in frustration. Toggling the
             * setting mid-alarm stops the chirp on the next tick.     */
            if (Settings_GetBuzzerEnabled() &&
                (fault_latched || overheat_latched)) {
                int phase = buzzer_tick % 10;
                if (phase == 0)      Buzzer_On();
                else if (phase == 2) Buzzer_Off();
                buzzer_tick++;
            } else {
                Buzzer_Off();
                buzzer_tick = 0;
            }
        } else {
            ESP_LOGW(TAG, "ADS1115 read failed");
            /* I2C bus hiccup, not a sender fault -- don't alarm. */
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
bool TempSender_Init(void)
{
    if (s_initialized) return true;

    /* Probe the ADS1115 by writing the config register. If nobody
     * ACKs at 0x48 we bail out so the rest of the app still runs. */
    esp_err_t err = ads_write_reg16(ADS1115_REG_CONFIG, ADS1115_CFG_SINGLE_A0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 not found on I2C at 0x%02X (err=%d)",
                 ADS1115_ADDR, err);
        return false;
    }

    BaseType_t ok = xTaskCreate(temp_task, "temp_sender", 4096,
                                NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn temp_task");
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "TempSender ready (ADS1115 @ 0x%02X, pullup=%.0f ohm)",
             ADS1115_ADDR, PULLUP_OHMS);
    return true;
}

float TempSender_GetTempF(void)         { return s_temp_f; }
float TempSender_GetResistanceOhms(void){ return s_resistance; }
