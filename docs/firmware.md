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
- **Shared I2C bus** — the RTC, GT911 touch controller and CH422G expander share one I2C bus (SDA GPIO8 / SCL GPIO9). The **display panel stack owns the legacy I2C driver** (`driver/i2c.h`) on host 0 — it is installed by `Board::begin()` with `ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST` / `ESP_PANEL_BOARD_EXPANDER_SKIP_INIT_HOST` at their default `0` in `include/esp_panel_board_custom_conf.h` — and the RTC is just another legacy-driver client on the same bus. Keep those flags at **0**: on arduino-esp32 ≥ 3.2 (ESP-IDF ≥ 5.4) Arduino `Wire` installs the *new* `i2c_master` driver, which cannot coexist with the legacy API the vendored panel libraries use — v1.6.0 briefly set the flags to 1 and began `Wire` first, which killed the CH422G (and with it LCD_RST + backlight) and produced a permanently black screen; v1.8.1 reverted to the panel-owned bus and switched the RTC to the legacy C API (`src/pcf85063.h`). The RTC probe therefore runs right after `Board::begin()` — still before the first LVGL frame renders.

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

## Optional wireless feed nodes (v1.8.0)

Battery "pucks" (any ESP8266/ESP32 + button) can trigger a feed wirelessly. Both radios implement the same **node datagram contract** ([`catfish_rfnode.h`](../firmware/common/catfish_rfnode.h)) and the display hub routes accepted requests through the *same* feed pipeline as touch/MQTT/dashboard — the relay unit is untouched.

| Transport | Feature flag | Status shown in Settings → RADIOS |
|-----------|--------------|-----------------------------------|
| **ESP-NOW** (hub is the receiver) | `-DCATFISH_FEATURE_ESPNOW=1` (default) | `listening` once WiFi connects (channel follows the AP — pucks must send on the same channel) |
| **nRF24L01+** (CE GPIO43 / CSN GPIO44, SPI shared with the TF card) | `-DCATFISH_FEATURE_NRF24=1` (default) | `listening` / `not found` when no module answers |

**Packet** (12 bytes): magic `'C''F'`, protocol version, command (`1` = feed), tank mask (0 = hub's current selection), seconds (0 = hub default pulse, hard-capped 60), a uint16 sequence number (repeated broadcasts are dropped), and a 4-byte key = FNV-1a of the **pairing token**. Trust model equals the LAN HTTP API: rotating the token invalidates every puck until it is re-provisioned (the hub re-keys automatically).

**Puck settings (nRF24)**: 1 Mbps, channel 76 (`-DCATFISH_RF24_CHANNEL`), 5-byte address `0xCA 0x74 0xF1 0x5B 0x01`. **Puck settings (ESP-NOW)**: broadcast the 12-byte datagram on the hub's WiFi channel.

**SPI sharing**: the nRF24 and the TF card sit on the same SPI pins; every SD and radio operation is serialized by `src/spi_bus_guard.h`, so both features can be enabled together. Remove a `-DCATFISH_FEATURE_*` line in `platformio.ini` to build a radio-free image — each feature degrades to a status line ("disabled"/"not found").

## Stability audit (v1.8.2)

A full-codebase audit and fix round. User-visible fixes:

- **Display tearing/flicker**: the anti-tearing configuration needs **two** RGB frame buffers, but only one was ever allocated (the vendor port expects the app to request the count before `Board::begin()`). LVGL then rendered straight into the buffer the panel was scanning — clock/countdown/toast updates sheared. Both buffers are now allocated (extra 768 KB in PSRAM).
- **MQTT could crash / never connect**: the MQTT library stores raw `const char*` config pointers; the hub passed temporary strings, which died at the end of `applyConfig()`. Config is now copied into stable buffers. Credentials can also be *cleared* (auth off) now, retained stray `ON` commands no longer replay a feed on every reconnect, and a disabled client no longer reports "offline - retrying".
- **RTC probe always failed**: the zero-byte I2C probe was rejected by ESP-IDF ≥ 5.4 (`i2c_master_write` rejects NULL data at any length), so a fitted CR927 was never detected. Fixed (1-byte register-pointer probe) — *this also silently fixes the v1.6.0 feature on hardware*.
- **Token rotation broke feeds until reboot**: the relay-token snapshot used for feeds was not refreshed after a successful rotation. It is now.
- **Relay unit safety**: changing the mechanism *during* a feed no longer latches the motor relay ON forever (stop path is now idempotent ALL-OFF, and `/config` rejects mechanism changes while feeding with `409 busy_feeding`); `/config` applies all-or-nothing (a bad `pulseS` no longer keeps a RAM-only new token).
- **Scheduled feeds are never silently dropped**: queue-full and partial multi-tank failures now arm the 30 s retry instead of vanishing with a toast.
- **Wireless feed nodes**: nRF24 hub now uses a static 12-byte payload (32-byte default desynchronized reads); the dedup ring no longer permanently rejects a node's very first `seq == 0` broadcast.
- **Dashboard**: schedule save no longer races its own re-fetch (looked like "save lost"); pulse stepper no longer snaps back within 3 s; token dialog no longer throws when already open; FEED omits `tanks` until the tank list has loaded (device default = all configured).
- **Lock hygiene**: `ui_set_pulse` (Home Assistant path) and three probe-result toasts now hold the LVGL lock; cross-task prefs/relay-IP string access is bounded and guarded (web task / LVGL task vs loop vs net-worker).
- **Misc**: constant-time dashboard token compare; OTA upload abort cleans the Update session; SD rotation counter starts at the existing file size (2 MB cap is real now); LVGL pool 64→96 KB (long-session "Out of memory" skips); `setEnabledByIdentity` (queued schedule toggles survive index shifts, unit-tested); mDNS on the relay is pumped explicitly instead of relying on ArduinoOTA; embedded dashboard gzip is byte-deterministic; web-flasher pins esp-web-tools 10.4.0; known-glyph placeholders replace two characters Montserrat renders as tofu.

## Stability fixes (v1.8.3)

Found by the new headless **QEMU smoke test** (see [tooling.md](tooling.md)) on
its very first run — both bugs are live on hardware, not emulator artifacts:

- **Endless event storm from programmatic spinbox updates** (LVGL 8.4 quirk):
  `lv_spinbox_set_value()` always fires `VALUE_CHANGED` because it ends in
  `lv_textarea_set_text()`, which re-emits the event unconditionally. Every
  programmatic update therefore re-entered the spinbox callbacks and
  re-enqueued their `UiEvent`s — two unbounded feedback loops:
  - the Settings tank-count spinbox rebuilt the whole tank UI in an endless
    boot loop (the serial log flooded with `tank count 1 -> 1` on every boot
    since v1.5.0, with a permanently re-triggered "Tanks set to N" toast),
  - every externally applied pulse (Home Assistant / dashboard) re-enqueued
    `SET_PULSE` forever after the first change — endless NVS flash writes and
    retained MQTT republishes since v1.3.0.
  All programmatic spinbox writes now go through a guarded quiet setter
  (`spinSetValueQuiet`), the two live callbacks ignore guarded updates, and
  the loop-side handlers treat same-value events as no-ops.
- **QEMU tooling**: `env:display-unit-qemu` + `scripts/qemu_run.sh` boot the
  full application headless in Espressif's QEMU (`esp32s3` machine), inject a
  wall clock, script real UI events through the production event queue and
  assert the effects (NVS persist, scheduler state, WiFi-down abort). Runs in
  CI on every push/PR as the "QEMU smoke test" job.

## Persistence & SD fixes (v1.8.4)

Field-reported: WiFi/time/feed-records "not saved after every power-off", and
"no sd card" in the Settings card. Two firmware root causes confirmed against
the library sources, plus hardening for marginal SD setups:

- **WiFi credentials lost on every power cycle** — `net_wifi.cpp` called
  `WiFi.persistent(false)` *before* the first `WiFi.mode(WIFI_STA)`. On
  arduino-esp32 the storage mode is decided exactly once, in
  `wifiLowLevelInit()` (`esp_wifi_set_storage(WIFI_STORAGE_RAM)` under
  `lowLevelInitDone`), so the whole session never wrote the STA config to
  NVS. WiFiManager 2.0.17's own temporary `WiFi.persistent(true)` around its
  `WiFi.begin()` could not undo that (the Arduino flag is not re-consulted),
  so every boot opened the setup portal again. Fix: keep persistence on
  (`WIFI_STORAGE_FLASH`); the driver writes NVS only when the config actually
  changes — no meaningful flash wear.
- **Time lost between sessions** was mostly a downstream effect: no saved
  WiFi → no NTP → the PCF85063 was never refreshed. The write-once-after-NTP
  trigger is now supplemented by an **hourly RTC rewrite** (drift correction;
  also recovers a first write that lost an I2C transaction to a concurrent
  touch poll). If the Settings card shows *battery dead/absent* after this
  release, the CR927 cell itself needs replacing — that part is hardware.
- **SD "no card" hardening**: the boot mount now retries at 20 → 10 → 4 MHz
  (marginal cards / slot wiring), and while no card is mounted the firmware
  **rechecks every 10 s** (one speed per pass, ≤ ~0.5 s loop cost) so a card
  inserted after boot — or one that needed the slower clock — mounts without
  a reboot; the Settings card label and a toast update on success. Cards
  must be FAT-formatted (FAT32 for ≤ 32 GB); if an SDXC card still refuses,
  re-format it FAT32 on a PC.

## Feeding feature pack (v1.9.0)

Implements the full v1.9.0 suggestion backlog (schedule/dosing, reliability,
insights, UX, integrations) **entirely on the display unit** — the relay wire
format ("feed for N seconds") and `CATFISH_PROTOCOL_VERSION 1` are untouched.

**Schedule & dosing**

- **Weekday masks (A1)** — every schedule entry carries a Mon–Sun mask
  (`d` in the schedule JSON, bit0=Mon..bit6=Sun; absent/0x7F = daily).
  `nextOccurrence()` scans up to 8 days ahead, so the countdown is correct
  even when the next feed is days out ("T+3d 07:30"). The editor (screen and
  dashboard) has day chips; empty mask clamps back to daily.
- **Per-tank seconds (A2)** — an entry can override the pulse per target tank
  (`st` = [t1..t4], 0 = inherit the shared seconds). Fan-out happens in the
  net worker (one `/feed` call per tank with its own seconds); no protocol
  change.
- **Portion presets (A3)** — S/M/L chips map to 2/5/10 s. Optional per-tank
  food-rate (g/s, TANKS editor) turns seconds into grams for the food-level
  readout.
- **Missed-feed catch-up (A4)** — while the unit was off/unreachable, missed
  entries inside the catch-up window (Settings > Feeding policy, default
  10 min, 0 = off) are fed once when the clock first becomes valid (RTC boot
  restore or first NTP sync). The fired-map (NVS) prevents double-fires.
- **Vacation mode (A5)** — Settings switch + Home badge. Pauses scheduled,
  catch-up and remote (web/MQTT) feeds; the on-device FEED NOW button and the
  wireless node buttons stay live (a human is present). Retained MQTT topic
  `<prefix>/paused`.

**Reliability**

- **Persistent retry journal (B1)** — a failed scheduled/catch-up feed is
  retried from an NVS-backed journal (2 slots, up to 3 retries at
  30 s / 5 min / 15 min backoff) that **survives power cycles**; blocked
  retries (e.g. vacation flipped on meanwhile) give up cleanly instead of
  looping.
- **RTC-time feeding (B2)** — scheduled feeds run on the battery-backed RTC
  whenever the wall clock is plausible; `/api/status` reports the source
  (`clockSrc`: ntp / rtc / none) and the Settings RTC line shows it.
- **Post-feed pulse verification (B3)** — for pulses ≥ 3 s a mid-pulse
  `/status` probe confirms the relay actually reports *feeding*; an
  online-but-not-pulsing relay raises a "not pulsing? (check food)" toast and
  a VERIFY row in the history (stall/empty-hopper hint).

**Insights & level**

- **On-device history (C1)** — Schedule > HISTORY shows the newest 24 events
  (OK/FAIL, tanks, seconds, manual/catch-up/retry/verify/blocked/skipped).
  RAM ring, works without an SD card (the SD CSV remains the durable copy).
- **`GET /api/history` + `GET /api/stats` (C2)** — the dashboard gains a
  history panel, "Today: N fed / M failed" and per-tank food bars.
- **Daily counters (C3)** — feeds/fails today + lifetime totals in NVS
  (day-rolled), mirrored to MQTT (`feeds_today`, `fails_today`).
- **Software hopper estimator (D1)** — per-tank "used seconds since refill"
  with an optional hopper size (TANKS editor) → level bar on Home, a FOOD
  card with per-tank refill buttons, a low-food threshold (red bar + MQTT
  `<prefix>/tankN/food`). The **hardware** level sensor (D2) remains a
  documented future item.

**UX**

- **Skip-next (E4)** — the "−" button on a schedule row skips that entry's
  next occurrence exactly once (persisted; consumed by the fire path).
- The next-feed countdown (E2), feed dose picker (E1: S/M/L + per-tank
  seconds) and near-duplicate schedule warning (E3, `hasNearDuplicate`) are
  part of the editor/list rework.

**Integrations & safety**

- **MQTT `feed/cmd` JSON (F1)** — accepts `{"seconds":1..30,"tanks":1..15}`
  next to the legacy `"ON"`; per-tank buttons unchanged.
- **Per-tank HA sensors (F2)** — `<prefix>/tankN/last_feed` (timestamp) +
  `<prefix>/tankN/seconds`, with auto-discovery.
- **Anti-spam guards (F3)** — minimum gap between feeds (minutes) and a
  max-feeds-per-day cap (Settings > Feeding policy; 0 = off). Applied to
  scheduled/catch-up/remote feeds; blocked attempts land in the history as
  *blocked*.

**Also fixed in v1.9.0 (found by the new test suite):** `tick()` fire-tracking
stored a minute-of-day stamp, so a daily entry never re-fired on later days of
one uptime (the catch-up scan masked it by feeding 0–10 min late). Fire stamps
are now absolute epoch-minutes — same HH:MM re-fires every day, still at most
once per calendar minute.

Native suite: **109 tests** (scheduler 40, feed_state 25, mqtt payloads 21,
mech/time/rfnode 23). QEMU smoke extended: vacation toggle + skip-next +
restore.

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
| Screen fully black, touch dead, `board begin() failed` on serial (v1.6.0–v1.7.0 regression, fixed in v1.8.1) | The v1.6.0 "Wire owns the I2C bus" experiment: Arduino Wire (new `i2c_master` driver on ESP-IDF ≥ 5.4) cannot coexist with the legacy driver API the CH422G/GT911 panel stack speaks → expander init fails → LCD_RST held, backlight off | Update to v1.8.1+ (panel owns the legacy bus again, RTC via legacy C API). If you must stay on an older build, re-flash v1.5.0 |
