# Firmware — Build, Flash, Configure

How to build, flash and configure both firmwares. For wiring see [hardware.md](hardware.md); for the relay unit's HTTP/UDP interface see [api.md](api.md).

## The two firmwares

| | Display Unit | Relay Unit |
|---|--------------|------------|
| Board | Waveshare ESP32-S3-Touch-LCD-4.3B (`esp32-s3-devkitc-1`) | ESP8266 NodeMCU V3 (`nodemcuv2`) |
| Platform | pioarduino 55.03.312 (Arduino core 3.x / ESP-IDF 5.5) | espressif8266@4.2.1 (pinned) |
| Framework libs | LVGL 8.4.0, ArduinoJson 7.4.2, WiFiManager 2.0.17 (pinned) + vendored ESP32_Display_Panel 1.0.0 / ESP32_IO_Expander 1.0.1 / esp-lib-utils 0.1.2 (in `lib/`) | ArduinoJson 7.4.2, WiFiManager 2.0.17 (pinned) |
| Flash layout | 16 MB, `default_16MB.csv` — bootloader `0x0`, partitions `0x8000`, boot_app0 `0xe000`, app `0x10000` | Single self-contained `firmware.bin` at `0x0` |
| Memory | `qio_opi`: 16 MB QIO flash + 8 MB octal PSRAM (`-DBOARD_HAS_PSRAM`) | 4 MB flash (board default) |
| Serial | 115200 baud (exception decoder enabled) | 115200 baud |

The display unit's panel library versions are vendored exactly as in Waveshare's official 4.3B demo package and must not be bumped casually — the LVGL port files (`src/lvgl_v8_port.cpp/h`) require the **LVGL v8 API**.

**Version single source:** both firmwares report the same version string from `firmware/common/catfish_protocol.h` (`CATFISH_APP_VERSION`). That header is also the single source of truth for discovery strings/ports, hostnames, token default/capacity, pulse limits and endpoint paths — change protocol constants there, never per-unit.

**Architecture (display unit):** the LVGL task renders/touches; a `wifi-task` owns WiFiManager/mDNS/NTP; a `net-worker` task (core 0) owns **all** blocking relay HTTP/discovery. `loop()` only enqueues commands and drains results through FreeRTOS queues, so the 1 Hz scheduler stays punctual during 6 s feed POSTs or 5.5 s discovery chains. UI callbacks never touch flash, the network, or the scheduler — they enqueue `UiEvent`s that `loop()` consumes.

## Prerequisites

- **VS Code + PlatformIO IDE extension** (easiest), or the CLI alone:
  ```sh
  pip install -U platformio
  ```
- **USB data cables** (not charge-only): USB-C for the display unit, micro-USB for the NodeMCU.
- **USB-serial drivers**: CH340 for most NodeMCU V3 boards (CP210x on some revisions); the display unit uses native USB (no driver needed on Windows 10+/macOS/Linux).
- **Chrome or Edge** if you want to flash from the browser (Web Serial).

Clone the repo:

```sh
git clone https://github.com/mlger/smartfishfeeder.git
cd smartfishfeeder
```

## Build

```sh
pio run -d firmware/display-unit     # ESP32-S3 UI
pio run -d firmware/relay-unit       # ESP8266 relay
```

The first build downloads the toolchains and registry libraries (LVGL, ArduinoJson, WiFiManager); the panel libraries are already vendored in `firmware/display-unit/lib/`. Artifacts land in `firmware/<unit>/.pio/build/<env>/` (`firmware.bin`, `bootloader.bin`, `partitions.bin` for the S3).

Library/platform versions are pinned (ArduinoJson 7.4.2, WiFiManager 2.0.17, espressif8266 4.2.1, CI uses PlatformIO 6.2.0) so builds are reproducible.

## Test (host-side, no hardware)

The feeding schedule engine is unit-tested on your machine with PlatformIO's native environment:

```sh
pio test -d firmware/display-unit -e native
```

This compiles the real `feed_scheduler.cpp` against a minimal `test/mocks/Arduino.h` with an injected clock (`feed_scheduler::set_time_source`) and covers add/duplicate rejection, clamping, capacity, once-per-minute firing, boot-inside-minute behaviour, disabled entries, NTP gating, today/tomorrow selection, identity removal and JSON persistence round-trips. CI runs the same suite in every build.

## Scheduled-feed resilience (display unit)

- A failed scheduled feed is retried **once** ~30 s later (two retry slots cover entries failing in the same window); the final failure raises a `Feed FAILED: <reason>` toast.
- A scheduled feed that fires while WiFi is down is not lost — it is retried ~60 s later when connectivity returns.
- Errors from the relay are mapped to human-readable toasts (`Token mismatch`, `Relay busy`, `Invalid duration`).
- A successful feed shows an optimistic "Feeding..." relay state for the pulse duration, then reconciles with the next real `/status` probe.

## Flash

Pick one of the three methods. All produce the same result.

### A. Web flasher (no toolchain)

1. Open **https://mlger.github.io/smartfishfeeder/** in Chrome or Edge.
2. Connect the unit by USB and click **Connect**.
3. Click **Install** and confirm. ESP Web Tools writes:
   - Display Unit — 4 images: bootloader `0x0`, partitions `0x8000`, boot_app0 `0xe000`, firmware `0x10000` (CI pre-merges them; you can also flash the released `merged-firmware.bin` as one image at `0x0` with esptool).
   - Relay Unit — one self-contained `firmware.bin` at `0x0`.
4. Wait for "successfully installed", then press RST (display unit) or replug (NodeMCU).

### B. esptool CLI

Display Unit (paths relative to `firmware/display-unit/.pio/build/display-unit/`; `boot_app0.bin` ships with the pioarduino platform under `packages/framework-arduinoespressif32/tools/partitions/`):

```sh
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash \
    0x0     bootloader.bin \
    0x8000  partitions.bin \
    0xe000  boot_app0.bin \
    0x10000 firmware.bin
```

Or flash the CI-merged whole-flash image in one shot:

```sh
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x0 merged-firmware.bin
```

Relay Unit:

```sh
esptool.py --chip esp8266 --port <PORT> --baud 460800 write_flash 0x0 firmware.bin
```

### C. PlatformIO upload

```sh
pio run -d firmware/display-unit -t upload
pio run -d firmware/relay-unit -t upload
```

If the ESP32-S3 upload fails to sync, hold the **BOOT** button while connecting USB-C (or while pressing RST), then retry.

### Erasing saved WiFi credentials

```sh
pio run -d firmware/display-unit -t erase   # or relay-unit
```

After an erase the WiFiManager portal comes up again on next boot.

## Serial monitor

```sh
pio device monitor -b 115200
```

The display unit build enables the ESP32 exception decoder, so backtraces are symbolized in the monitor. Exit with `Ctrl+C`.

## WiFi provisioning

Both units use WiFiManager. On first boot (or after `erase`) they:

1. Start an access point and captive portal:
   - Display Unit: AP **`CatfishFeeder-Setup`**
   - Relay Unit: AP **`CatfishRelay-Setup`**
2. Join the AP from a phone/laptop — the config portal opens automatically (or browse to `192.168.4.1`).
3. Select your 2.4 GHz SSID, enter the passphrase, save. The unit reboots onto your network.
4. The relay unit then announces itself as **`catfish-relay.local`** (mDNS) and answers UDP discovery on port 5050 — see [api.md](api.md).

If the saved WiFi is unavailable, the units fall back to their setup APs automatically.

## Configuration

| Setting | Where | Default | How to change |
|---------|-------|---------|---------------|
| Feed token | Relay Unit | `catfish` | Display UI: Settings → Relay Unit → *Pairing token* (tick "Also update the relay unit" to rotate both sides), or `POST /config` with `{"token":"..."}` |
| Relay active level | Relay Unit `config.h` | board-dependent | `RELAY_ACTIVE_LEVEL` compile-time constant — set to match your relay board (many are ACTIVE-LOW), re-flash |
| Timezone | Display Unit | Asia/Manila | POSIX TZ string `PHT-8` (UTC+8, no DST); schedule times are local |
| NTP | Display Unit | pool servers | Synced periodically; UI shows synced time |
| Battery clock | Display Unit | on (auto-detected) | PCF85063 + CR927 coin cell keeps time through power loss (v1.6.0) — see [Battery-backed RTC](#battery-backed-rtc-v160); status in Settings → System |
| Clock format | Display Unit | 24-hour | Settings → Display → *12-hour clock* switch (v1.6.0, persisted). Presentation only — schedules, MQTT and the HTTP API stay 24-hour |
| SD feed log | Display Unit | auto-detected | Insert a microSD card (FAT32/exFAT); feed history lands in `/feeds.csv`. Compile-out with `-DCATFISH_FEATURE_SD` removal — see [Optional SD card](#optional-sd-card-v170) |
| Backlight | Display Unit | on | Settings → Display → Backlight switch (persisted across reboots); screen auto-dims after 10 min idle and wakes on touch |
| MQTT / Home Assistant | Display Unit | disabled | Settings → HOME ASSISTANT (MQTT) → *Configure*: broker, port, credentials, topic prefix — topic map and behaviour in [mqtt.md](mqtt.md) |
| Feeding mechanism | Relay Unit | `motor` | Display UI: Settings → Relay Unit → *Mechanism* → *Configure* (motor / servo / stepper + servo angles / stepper rate), or `POST /config` with `{"mechanism":...}` — semantics and wiring in [api.md](api.md#feeding-mechanisms-v140). Stored in a separate EEPROM record; factory reset restores `motor` |
| Tanks (multi-relay) | Display Unit | 1 | Display UI: Settings → TANKS → count spinbox (1-4), *Scan for relays*, per-tank Edit (name + token override) — architecture and binding rules in [api.md](api.md#multi-tank-v150) |
| Factory reset | Relay Unit | — | Hold the **FLASH** button ~5 s while running: EEPROM config (token, pulse caps, mechanism) is wiped to defaults and the unit reboots. Refused while a pulse is running |

**Timezone note:** the firmware uses the POSIX form `PHT-8` — a positive UTC offset is written with a minus sign in POSIX. Schedule entries entered on the Schedule screen are interpreted in this timezone.

**Token note:** `catfish` is a default, not a feature. Change it before going live — the easiest way is the display unit itself (Settings → Relay Unit → *Pairing token*, tick **"Also update the relay unit"**): it rotates the relay token via `POST /config` and only saves the new token locally after the relay accepted it. To rotate without the display: `curl -X POST http://catfish-relay.local/config -H 'Content-Type: application/json' -d '{"token":"your-current-token","newToken":"your-new-token"}'`, then update the display's token in Settings. Token changes take effect immediately and survive reboots.

## Feeding mechanisms (v1.4.0+)

The relay unit ships with the classic **motor** mechanism (D1 relay/FET energizes a DC auger/roller motor for the pulse). Two alternative mechanisms reuse the *same* "feed for N seconds" API — only what the ESP8266 does during those seconds differs:

| Mechanism | Relay-unit pins | Notes |
|-----------|-----------------|-------|
| `motor` (default) | D1 → relay module input | unchanged behaviour; 1N4007 flyback diode across the motor |
| `servo` | **D5** (GPIO14) → servo signal; servo +5 V / GND | SG90-class. Sweeps rest ↔ feed angle every 0.5 s during the pulse, parks at rest and detaches after each feed |
| `stepper` | **D2** (GPIO4) → driver STEP; **D6** (GPIO12) → driver DIR | A4988/DRV8825-class. STEP pulses at the configured rate (default 400 steps/s); DIR is driven low (or high when enabled) for the pulse. Tie `/ENABLE`+`/RESET`+`/SLEEP` per the driver datasheet, set MS1-MS3 for microstepping, and fit a ≥100 µF decoupling cap on VMOT |

Switch mechanisms on the display unit (Settings → Relay Unit → *Mechanism*) or via `POST /config` (see [api.md](api.md)). The choice + parameters persist in a second additive EEPROM record; upgrading the firmware never touches the token/pulse blob. The STEP/DIR pins are driven low from the first boot microseconds, so a partially-wired stepper driver stays idle until you deliberately switch mechanisms.

## Multi-tank / multi-relay (v1.5.0+)

One display unit can drive up to **4 relay units** (one per tank). The relay firmware is unchanged — each relay stays a standalone device with its own token, and the display learns their IPs:

- **Configuration**: Settings → TANKS. The count spinbox (1-4) controls how many tanks exist; each tank gets a display name and an optional per-tank **token override** (empty = inherit the shared pairing token).
- **Binding**: every tank keeps its own last-known relay IP (persisted to NVS). *Scan for relays* sends one UDP broadcast round and binds every tank without a live IP to a relay not already claimed by another tank. Per-tank discovery (first feed/probe after an IP loss) reuses the same rule, so two tanks never auto-bind to the same relay.
- **Schedules**: each entry carries a target-tank bitmask (default: every configured tank for new entries; pre-v1.5.0 entries load as tank 1). The editor shows tank checkboxes when 2+ tanks are configured.
- **Feeding**: FEED NOW targets the selected tank chip (Home screen) or "All"; the worker feeds each target tank sequentially and toasts partial failures per tank.
- **Sensors**: Home Assistant gains a feed button + online sensor per tank; the web dashboard shows per-tank chips and status.
- **Single-tank setups are unchanged**: with the count at 1 there are no chips, no checkboxes, and behavior is byte-for-byte the pre-v1.5.0 flow (the legacy `relayIp` NVS key is migrated to tank 1 on first boot).

Multi-relay caveat: all relay units share the `catfish-relay` mDNS hostname, so UDP broadcast is the authoritative multi-relay discovery (mDNS remains a fallback candidate for single-tank setups). If two relays answer UDP from the same broadcast domain, each tank binds to a different one automatically.

## Battery-backed RTC (v1.6.0)

The display board carries an **NXP PCF85063** real-time clock backed by a **CR927 coin cell** (holder on the board rear). Before v1.6.0 the chip sat unused: after a power loss the device booted clock-less and stayed that way until NTP synced — schedules didn't fire and *last fed* was unknown on offline boots.

What the firmware does now:

- **Boot restore** — if the chip holds a plausible time (≥ 2024), it is applied via `settimeofday()` *before* the UI starts, so schedules and the clock are correct immediately, even with no WiFi/NTP.
- **NTP write-back** — after the first real NTP sync each boot, the wall clock is written back into the chip (clears the oscillator-stop flag), keeping the CR927 time fresh.
- **Status** — Settings → System shows the RTC state: `battery OK`, `battery dead/absent` (oscillator-stop flag latched → replace the CR927), `time never set` (chip alive but no valid time yet), or `not present`.
- **Shared I2C bus** — the RTC, GT911 touch controller and CH422G expander share one I2C bus (SDA GPIO8 / SCL GPIO9). Arduino `Wire` owns the legacy I2C driver and the panel stack attaches to it (`ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST` / `ESP_PANEL_BOARD_EXPANDER_SKIP_INIT_HOST` = 1 in `include/esp_panel_board_custom_conf.h`). Do **not** set those flags back to 0 — the panel would try to re-install the driver Wire already owns and the touch/expander would fail.

All stored/derived times are UTC internally; the `PHT-8` timezone applies at render time only.

## Optional SD card (v1.7.0)

The board's TF slot (SPI: MOSI GPIO11, SCK GPIO12, MISO GPIO13, card CS on expander **EXIO4**) is used as an optional **feed-history log**:

- **Compile-time optional**: enabled by `-DCATFISH_FEATURE_SD=1` in `firmware/display-unit/platformio.ini`. Remove that line and rebuild for an SD-free image.
- **Runtime-optional**: no card in the slot just reports `no card` in Settings → STORAGE — everything else behaves identically.
- **What gets logged**: every feed result (manual, scheduled, HA, dashboard — successes *and* failures) appends one line to `/feeds.csv`: `epoch,iso8601,tankMask,seconds,ok`. Rows are only written once the clock is valid (RTC or NTP).
- **Rotation**: when `/feeds.csv` passes 2 MB it is renamed to `/feeds.old.csv` (previous copy replaced) and a fresh file starts.
- **Reading the log**: eject the card and open it on a PC, or `cat` it over the serial console — no HTTP endpoint by design (LAN appliance keeps its surface small).
- **Bus etiquette**: the card's CS is asserted only around each SD operation, so the SPI bus is free between writes (relevant when the optional nRF24L01 shares the same pins in v1.8.0+).

Mount happens once in `setup()` (blocking ~100–300 ms, before the UI comes up). Cards ≥ 8 GB formatted FAT32/exFAT are fine.

## OTA updates

**Both units run ArduinoOTA**, so firmware updates need no cable once they are on the network:

```sh
pio run -d firmware/relay-unit -t upload --upload-port catfish-relay.local     # ESP8266, port 8266
pio run -d firmware/display-unit -t upload --upload-port catfishfeeder.local   # ESP32-S3, port 3232
```

The display unit begins its OTA service lazily once WiFi/mDNS is up and advertises the standard `_arduino._tcp` service as `catfishfeeder` — it shows up in IDE/PlatformIO device lists like the relay does. While an update is in flight the screen shows a *Receiving update...* toast; the unit reboots itself when the transfer completes.

Prefer a browser? The **web dashboard** (http://catfishfeeder.local/) can also flash `firmware.bin` from any phone — *System → Update firmware*. Both paths write to the same OTA slot; don't run them simultaneously. Details in [dashboard.md](dashboard.md).

Requirements: PC and unit on the same LAN, mDNS resolution working (`.local`), and the relay unit idle (the display unit can stay in normal operation).

OTA is **open (no password) by default** — a documented decision for this LAN-only appliance. To require a password, add the credential at build time (per unit):

```sh
pio run -d firmware/relay-unit -t upload --upload-port catfish-relay.local \
    --build-flag '-DOTA_PASSWORD="s3cret"'
pio run -d firmware/display-unit -t upload --upload-port catfishfeeder.local \
    --build-flag '-DOTA_PASSWORD="s3cret"'
```

(or add `-DOTA_PASSWORD=\"s3cret\"` to `build_flags` in the unit's `platformio.ini` — both `main.cpp` files fall back to an empty password when the flag is absent).

## Partitions & PSRAM notes

- Flash: 16 MB QIO (`board_upload.flash_size = 16MB`, `board_build.flash_mode = qio`), partition table `default_16MB.csv` — large OTA-capable app slots, SPIFFS area, and the `boot_app0.bin` OTA-data sector at `0xe000` that selects the running slot.
- PSRAM: 8 MB octal on the N16R8 module — requires `board_build.arduino.memory_type = qio_opi` and `-DBOARD_HAS_PSRAM`. Wrong memory type is the usual cause of PSRAM init failures.
- LVGL pool: `include/lv_conf.h` sets `LV_MEM_SIZE` to 64 KB with the Montserrat fonts needed by the UI already enabled.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| White screen on the display unit | Panel not initialized (bad board config or boot loop before LVGL starts) | Check serial logs; verify `include/esp_panel_board_custom_conf.h` was not modified; re-flash all four images at the correct offsets |
| No touch response | GT911 reset/int sequence handled via CH422G expander | Confirm vendored libs are 1.0.0/1.0.1 (not registry variants); check logs for `GT911`/expander init errors; power-cycle fully |
| Relay not found by display unit | Different subnet/AP isolation, mDNS blocked, relay offline | Ping `catfish-relay.local`; test UDP discovery (see [api.md](api.md)); ensure both units are on the same 2.4 GHz SSID and that client isolation is off. A stale saved IP self-heals within ~2–3 min (3 failed probes drop it) — or press *Test connection* which re-runs discovery |
| Setup portal not appearing | Credentials already saved, or portal only shows when WiFi fails | Erase flash (`pio run -d <dir> -t erase`) and re-provision; check the phone actually joined the setup AP |
| `PSRAM init failed` at boot | Wrong memory type or missing build flag | Keep `qio_opi` and `-DBOARD_HAS_PSRAM` in `firmware/display-unit/platformio.ini`; verify you have the N16R8 module (8 MB PSRAM) |
| WiFi keeps dropping | Weak 2.4 GHz signal, PSU sag, RF congestion | Move closer to the AP, check the 5 V rail under motor load (brown-outs reset the NodeMCU), pick a clearer channel |
| Upload "Failed to connect" (S3) | Board not in download mode | Hold BOOT while connecting USB-C, then retry; use a data cable |
| Upload fails (NodeMCU) | CH340 driver missing / wrong port | Install the CH340 driver, pick the correct `<PORT>` (`pio device list`), close other serial monitors |
| Motor runs at boot / continuously | Relay board is ACTIVE-LOW with mismatched `RELAY_ACTIVE_LEVEL` | Flip `RELAY_ACTIVE_LEVEL` in `config.h`, re-flash; verify idle state before leaving unattended |
| Periodic display flicker / shimmer (v1.6.1 guidance) | RGB signal integrity or power rail sag — the LVGL config itself (direct-mode + double frame buffer) is the vendor-recommended one | (1) Power the display unit from a solid 5 V ≥ 1 A source (long/thin USB cables cause rail sag → flicker); (2) re-seat the panel FPC connector; (3) rebuild with a lower pixel clock: add `-DCATFISH_LCD_PCLK_HZ=12000000` (or `8000000`) to `firmware/display-unit/platformio.ini` `build_flags`; (4) if flicker only happens right after an idle-dim wake or toggle, report it — the backlight is a plain expander switch, so a repeated on/off flapping indicates the inactivity timer oscillating, not panel hardware |
