# Hardware Notes — Waveshare ESP32-S3-Touch-LCD-4.3B (VERIFIED)

> Source of truth: Waveshare official wiki (docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B)
> and the official demo package `ESP32-S3-Touch-LCD-4.3B-BOX-Demo.zip`
> (downloaded from files.waveshare.com, verified 2025).
> Do NOT guess pins — everything below is hardware-verified.

## Board summary

| Item | Value |
|---|---|
| MCU module | ESP32-S3-WROOM-1-N16R8 (dual-core 240 MHz) |
| Flash | 16 MB (QIO) |
| PSRAM | 8 MB (octal, `qio_opi` memory type) |
| Display | 4.3" IPS, 800x480, RGB565 parallel (ST7262-class panel) |
| Touch | Capacitive 5-point, GT911 (I2C) |
| IO expander | CH422G (I2C) — controls LCD reset, touch reset, backlight(DISP), SD CS |
| USB | Type-C (native USB, power + flashing) |
| Extra | CAN (TWAI), RS485, TF card, RTC PCF85063, 7-36V DC input, 3.7V Li-ion |
| Power | 5V via USB-C, ~450 mA typical |

## RGB LCD pins (verified, matches demo `esp_panel_board_custom_conf.h`)

Control:
- HSYNC = GPIO46
- VSYNC = GPIO3
- DE    = GPIO5
- PCLK  = GPIO7
- LCD reset = via CH422G expander pin 3 (toggled low→high in LCD pre-begin)

Data bus (16-bit RGB565, DATA0..DATA15 in bus-bit order):

| Bus bit | Signal | GPIO |
|---|---|---|
| D0  | B3 | GPIO14 |
| D1  | B4 | GPIO38 |
| D2  | B5 | GPIO18 |
| D3  | B6 | GPIO17 |
| D4  | B7 | GPIO10 |
| D5  | G2 | GPIO39 |
| D6  | G3 | GPIO0  |
| D7  | G4 | GPIO45 |
| D8  | G5 | GPIO48 |
| D9  | G6 | GPIO47 |
| D10 | G7 | GPIO21 |
| D11 | R3 | GPIO1  |
| D12 | R4 | GPIO2  |
| D13 | R5 | GPIO42 |
| D14 | R6 | GPIO41 |
| D15 | R7 | GPIO40 |

Timing (from Waveshare demo, 800x480 branch):
- PCLK: 16 MHz, active on falling edge (`pclk_active_neg = 1`)
- H: pulse width 4, back porch 8, front porch 8
- V: pulse width 4, back porch 16, front porch 16
- LV_COLOR_16_SWAP = 0, RGB order (not BGR)

## Touch (GT911)

- I2C: SDA = GPIO8, SCL = GPIO9 (400 kHz, pullups on)
- INT = GPIO4 (active low; held low during expander-driven reset, then released)
- Reset = CH422G expander pin 1 (EXIO1): low 100 ms → high 200 ms
- I2C address: default 0x5D (GT911 also supports 0x14; driver probes)

## CH422G IO expander

- I2C: shared bus SDA = GPIO8, SCL = GPIO9, 400 kHz
- Configured address in demo: 0x20 (ESP32_IO_Expander handles register map)
- `enableAllIO_Output()` called post-begin (required)
- Pin map (expander index):
  - pin 1 (EXIO1) = touch reset (TP_RST)
  - pin 2 (EXIO2) = DISP / backlight enable (HIGH = ON)
  - pin 3 (EXIO3) = LCD reset (per demo pre-begin function)
  - pin 4 (EXIO4) = TF card CS (active low)
- Backlight is a switch (expander), NOT PWM — brightness via LEDC not available
  on this board; dimming must be implemented differently (or on/off only).

## Other pins

- TF card: MOSI=GPIO11, SCK=GPIO12, MISO=GPIO13, CS=expander EXIO4
- RS485: RXD=GPIO43, TXD=GPIO44
- CAN/TWAI: TX=GPIO15, RX=GPIO16
- RTC PCF85063: on shared I2C (GPIO8/9)
- USB: D- = GPIO19, D+ = GPIO20 (do not use these GPIOs)

## Software stack (vendor-verified versions)

| Library | Version | Origin |
|---|---|---|
| ESP32_Display_Panel | 1.0.0 | vendored in `lib/` |
| ESP32_IO_Expander | 1.0.1 | vendored in `lib/` |
| esp-lib-utils | 0.1.2 | vendored in `lib/` |
| LVGL | 8.4.0 | PlatformIO registry (lv_conf.h tuned in `include/`) |
| Board port files | — | Waveshare demo `lvgl_v8_port.cpp/h` in `src/` |
| Arduino core | 3.0.7+ (demo), we use pioarduino 55.03.312 (core 3.3.x) | platformio.ini |
| ESP-IDF (reference) | 5.5.2 | Waveshare ESP-IDF guide |

LVGL v8 API is REQUIRED (port files are v8): `lv_scr_act()`, `lv_btn_create()`,
`lvgl_port_lock(-1)` / `lvgl_port_unlock()` around LVGL calls from non-LVGL tasks.

## Feeder actuation side

- ESP8266 NodeMCU V3: relay driver on D1 (GPIO5), status LED on D4 (GPIO2, active low)
- Generic 5V 1CH optocoupler relay module (many are ACTIVE-LOW; configurable in firmware)
- Feeder motor: 5V DC (auger/roller), wired through relay NO contact in series with +5V
- Flyback diode (1N4007) across motor terminals, common GND between units
