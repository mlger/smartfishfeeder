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
 * Bus ownership (important, v1.8.1): the display panel stack installs and
 * owns the LEGACY I2C driver (driver/i2c.h) on host 0 when Board::begin()
 * runs — the board config keeps TOUCH/EXPANDER SKIP_INIT_HOST at 0. Arduino
 * Wire must NOT be used on this board: on arduino-esp32 >= 3.2 (ESP-IDF
 * >= 5.4) Wire installs the NEW i2c_master driver, which cannot coexist with
 * the legacy API the vendored panel libraries use (v1.6.0 tried "Wire owns
 * the bus" and the CH422G — which gates LCD_RST and the backlight — went
 * dead, leaving a permanently black screen). This driver therefore speaks
 * the same legacy C API as the panel stack; the legacy driver happily serves
 * multiple devices on one bus. ALL functions here must be called only AFTER
 * Board::begin() has succeeded (setup() enforces the ordering). Occasional
 * RTC transactions coexist with GT911 polling through the legacy driver's
 * internal port mutex.
 *
 * All times are UTC; the TZ ("PHT-8") applied by configTzTime only affects
 * localtime rendering, never what we store.
 */
#ifndef CATFISH_PCF85063_H
#define CATFISH_PCF85063_H

#include <Arduino.h>
#include <driver/i2c.h>
#include <time.h>
#include <stdint.h>
#include <catfish_time.h>

namespace pcf85063 {

static const uint8_t I2C_ADDR = 0x51;
static const i2c_port_t I2C_NUM = I2C_NUM_0; // host 0 — panel-owned legacy driver
static const TickType_t I2C_TIMEOUT = pdMS_TO_TICKS(100);

// Register map (subset we use)
static const uint8_t REG_CTRL1 = 0x00;
static const uint8_t REG_SEC = 0x02; // seconds..years run 0x02..0x08
static const uint8_t N_DATETIME_REGS = 7;

// Bits
static const uint8_t SEC_OS_MASK = 0x80;  // seconds bit7: oscillator has stopped
static const uint8_t MONTH_CENTURY = 0x80; // months bit7: 0 = 20xx, 1 = 19xx
static const uint8_t HOUR_12H = 0x80;     // hours bit7 must stay 0 (24h mode)

/**
 * @brief Probe the chip (address phase only). Returns false until the panel
 *        stack has installed the legacy I2C driver, i.e. call only after
 *        Board::begin() succeeded. Returns true when the chip ACKs (battery
 *        holder populated).
 */
inline bool begin() {
  return i2c_master_write_to_device(I2C_NUM, I2C_ADDR, nullptr, 0,
                                    I2C_TIMEOUT) == ESP_OK;
}

/** @return true when the chip answered the probe in begin(). */
inline bool isPresent() {
  return begin();
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

  // Set the register pointer to seconds, then read the 7 calendar regs.
  // (Write-then-read, no repeated start — the PCF85063 doesn't need one.)
  const uint8_t reg = REG_SEC;
  if (i2c_master_write_to_device(I2C_NUM, I2C_ADDR, &reg, 1, I2C_TIMEOUT) !=
      ESP_OK)
    return false;
  uint8_t raw[N_DATETIME_REGS];
  if (i2c_master_read_from_device(I2C_NUM, I2C_ADDR, raw, N_DATETIME_REGS,
                                  I2C_TIMEOUT) != ESP_OK)
    return false;

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
  return i2c_master_write_to_device(I2C_NUM, I2C_ADDR, buf, sizeof(buf),
                                    I2C_TIMEOUT) == ESP_OK;
}

} // namespace pcf85063

#endif // CATFISH_PCF85063_H
