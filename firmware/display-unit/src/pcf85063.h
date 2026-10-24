/**
 * @file pcf85063.h
 * @brief Battery-backed real-time clock (NXP PCF85063) on the 4.3B board.
 *
 * The board's CR927 cell keeps the PCF85063 (I2C address 0x51, shared bus on
 * SDA=GPIO8 / SCL=GPIO9, same host as GT911 + CH422G) ticking while main
 * power is off. Until v1.6.0 the firmware ignored the chip entirely: after a
 * power loss the display booted with no valid wall clock until NTP synced,
 * so schedules didn't fire and "last fed" stayed unknown.
 *
 * Bus ownership (important): Arduino Wire owns the legacy I2C driver on
 * I2C_NUM_0 (installed by begin() BEFORE the display panel stack starts).
 * The board config sets ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST and
 * ESP_PANEL_BOARD_EXPANDER_SKIP_INIT_HOST to 1 so GT911/CH422G attach to the
 * same already-running driver instead of re-installing it. That makes RTC
 * access safe at ANY time from setup() or loop() — one driver, one bus,
 * mutex-protected by the legacy I2C layer.
 *
 * All times are UTC; the TZ ("PHT-8") applied by configTzTime only affects
 * localtime rendering, never what we store.
 */
#ifndef CATFISH_PCF85063_H
#define CATFISH_PCF85063_H

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <stdint.h>
#include <catfish_time.h>

namespace pcf85063 {

static const uint8_t I2C_ADDR = 0x51;

// Register map (subset we use)
static const uint8_t REG_CTRL1 = 0x00;
static const uint8_t REG_SEC = 0x02; // seconds..years run 0x02..0x08
static const uint8_t N_DATETIME_REGS = 7;

// Bits
static const uint8_t SEC_OS_MASK = 0x80;  // seconds bit7: oscillator has stopped
static const uint8_t MONTH_CENTURY = 0x80; // months bit7: 0 = 20xx, 1 = 19xx
static const uint8_t HOUR_12H = 0x80;     // hours bit7 must stay 0 (24h mode)

/**
 * @brief Take the shared I2C bus (Wire.begin on SDA 8 / SCL 9) and probe the
 *        chip. Safe to call before the panel stack starts.
 * @return true when the chip ACKs its address (battery holder populated).
 */
inline bool begin() {
  Wire.begin(8, 9, 400000U);
  Wire.beginTransmission(I2C_ADDR);
  return Wire.endTransmission() == 0;
}

/** @return true when the chip answered the probe in begin(). */
inline bool isPresent() {
  Wire.beginTransmission(I2C_ADDR);
  return Wire.endTransmission() == 0;
}

static uint8_t bcdToBin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }
static uint8_t binToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/**
 * @brief Read the calendar registers and convert to a UTC epoch.
 * @param[out] out        UTC epoch seconds.
 * @param[out] oscStopped set when the oscillator-stop flag is latched, i.e.
 *                        the backup battery failed/was absent and the stored
 *                        time is stale.
 * @return false on I2C error or an impossible register value (e.g. month 0).
 */
inline bool readUtc(time_t *out, bool *oscStopped) {
  if (!out || !oscStopped) return false;
  *oscStopped = false;
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(REG_SEC);
  if (Wire.endTransmission(false) != 0) return false; // repeated start
  if (Wire.requestFrom((int)I2C_ADDR, N_DATETIME_REGS) != N_DATETIME_REGS)
    return false;

  uint8_t raw[N_DATETIME_REGS];
  for (int i = 0; i < N_DATETIME_REGS; i++) raw[i] = (uint8_t)Wire.read();

  const uint8_t sec = bcdToBin(raw[0] & 0x7F);
  const uint8_t min = bcdToBin(raw[1] & 0x7F);
  const uint8_t hour = bcdToBin(raw[2] & 0x3F); // bit7 = 12h flag (unused)
  const uint8_t day = bcdToBin(raw[4] & 0x3F);
  const uint8_t mon = bcdToBin(raw[5] & 0x1F);
  const uint8_t yr = bcdToBin(raw[6]);
  if (raw[0] & SEC_OS_MASK) *oscStopped = true;
  if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 ||
      sec > 59)
    return false;

  CatfishCivil c;
  c.year = (raw[5] & MONTH_CENTURY) ? 1900 + yr : 2000 + yr;
  c.month = mon;
  c.day = day;
  c.hour = hour;
  c.minute = min;
  c.second = sec;
  *out = (time_t)catfishTimeCivilToEpoch(c);
  return true;
}

/**
 * @brief Write the current time into the chip (also clears the OS flag).
 *        Years outside 2000..2099 are clamped (chip is a 2-digit-year BCD).
 * @return false on I2C error.
 */
inline bool writeUtc(time_t t) {
  CatfishCivil c = catfishTimeEpochToCivil((int64_t)t);
  if (c.year < 2000) c.year = 2000;
  if (c.year > 2099) c.year = 2099;

  const uint8_t buf[N_DATETIME_REGS + 1] = {
      REG_SEC,
      (uint8_t)(binToBcd(c.second) & (uint8_t)~SEC_OS_MASK), // clears OS flag
      binToBcd(c.minute),
      (uint8_t)(binToBcd(c.hour) & (uint8_t)~HOUR_12H), // keep 24-hour mode
      0x00,                                             // weekday (unused by us)
      binToBcd(c.day),
      (uint8_t)(binToBcd(c.month) & (uint8_t)~MONTH_CENTURY), // century 0 = 20xx
      binToBcd((uint8_t)(c.year - 2000)),
  };
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(buf, sizeof(buf));
  return Wire.endTransmission() == 0;
}

} // namespace pcf85063

#endif // CATFISH_PCF85063_H
