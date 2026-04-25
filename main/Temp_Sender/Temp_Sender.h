/*
 * Temp_Sender.h -- Prosport PSOWTS-JPN coolant temperature sender driver.
 *
 * Reads a Prosport PSOWTS-JPN (NTC thermistor, two-wire) through an
 * ADS1115 16-bit I2C ADC. The ADS1115 sits on the board's I2C bus
 * (same SCL/SDA as touch/IMU/RTC, at address 0x48 by default).
 *
 * Circuit:
 *   3.3V --[ 1kohm pullup ]--+-- ADS1115 A0
 *                            |
 *                       [ Prosport ]
 *                            |
 *                           GND  (engine block / chassis)
 *
 * Factory spec (7 points, see Temp_Sender.c PROSPORT_CURVE[]):
 *   104 F -> 5830 ohm  (~2.09 V at ADC)
 *   212 F ->  975 ohm  (~1.62 V)
 *   302 F ->  316 ohm  (~0.79 V)
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
