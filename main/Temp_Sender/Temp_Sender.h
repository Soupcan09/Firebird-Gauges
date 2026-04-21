/*
 * Temp_Sender.h -- GM TS6 coolant temperature sender driver.
 *
 * Reads a GM TS6 (NTC thermistor) through an ADS1115 16-bit I2C ADC.
 * The ADS1115 sits on the board's I2C bus (same SCL/SDA as touch/IMU/RTC,
 * at address 0x48 by default).
 *
 * Circuit:
 *   3.3V --[ 1kohm pullup ]--+-- ADS1115 A0
 *                            |
 *                          [ TS6 ]
 *                            |
 *                           GND  (engine block / chassis)
 *
 * Cold sender is ~1365 ohm  => ~1.90 V at ADC input (100 F)
 * Hot  sender is    ~55 ohm => ~0.17 V at ADC input (260 F)
 *
 * Call TempSender_Init() once from app_main after Wireless/LVGL init,
 * then a background task periodically pushes readings into
 * set_gauge_temp_f() so the needle follows the sender.
 */
#ifndef TEMP_SENDER_H
#define TEMP_SENDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Initialize the ADS1115 and spawn the background sampling task.
 * Returns true on success, false if the ADS1115 didn't respond on I2C. */
bool TempSender_Init(void);

/* Most-recent averaged temperature in Fahrenheit. Useful for logging
 * or for an on-screen readout later. */
float TempSender_GetTempF(void);

/* Most-recent raw resistance of the sender in ohms. Handy while you're
 * bench-calibrating with a pot of water + thermometer. */
float TempSender_GetResistanceOhms(void);

#ifdef __cplusplus
}
#endif

#endif /* TEMP_SENDER_H */
