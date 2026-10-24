# Firmware — Build, Flash, Configure

How to build, flash and configure both firmwares. For wiring see [hardware.md](hardware.md); for the relay unit's HTTP/UDP interface see [api.md](api.md).

## The two firmwares

| | Display Unit | Relay Unit |
|---|--------------|------------|
| Board | Waveshare ESP32-S3-Touch-LCD-4.3B (`esp32-s3-devkitc-1`) | ESP8266 NodeMCU V3 (`nodemcuv2`) |
| Platform | pioarduino 55.03.312 (Arduino core 3.x / ESP-IDF 5.5) | espressif8266@^4.2.1 |
| Framework libs | LVGL 8.4.0, ArduinoJson ^7.4.2, WiFiManager ^2.0.17 + vendored ESP32_Display_Panel 1.0.0 / ESP32_IO_Expander 1.0.1 / esp-lib-utils 0.1.2 (in `lib/`) | ArduinoJson ^7.4.2, WiFiManager ^2.0.17 |
| Flash layout | 16 MB, `default_16MB.csv` — bootloader `0x0`, partitions `0x8000`, boot_app0 `0xe000`, app `0x10000` | Single self-contained `firmware.bin` at `0x0` |
| Memory | `qio_opi`: 16 MB QIO flash + 8 MB octal PSRAM (`-DBOARD_HAS_PSRAM`) | 4 MB flash (board default) |
| Serial | 115200 baud (exception decoder enabled) | 115200 baud |

The display unit's panel library versions are vendored exactly as in Waveshare's official 4.3B demo package and must not be bumped casually — the LVGL port files (`src/lvgl_v8_port.cpp/h`) require the **LVGL v8 API**.

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

## Flash

Pick one of the three methods. All produce the same result.

### A. Web flasher (no toolchain)

1. Open **https://mlger.github.io/smartfishfeeder/** in Chrome or Edge.
2. Connect the unit by USB and click **Connect**.
3. Click **Install** and confirm. ESP Web Tools writes:
   - Display Unit — 4 images: bootloader `0x0`, partitions `0x8000`, boot_app0 `0xe000`, firmware `0x10000` (CI pre-merges them; you can also flash the released `merged_firmware.bin` as one image at `0x0` with esptool).
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
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x0 merged_firmware.bin
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
| Feed token | Relay Unit | `catfish` | `POST /config` with `{"token":"..."}` (persisted to flash), or edit the default in `config.h` and re-flash |
| Relay active level | Relay Unit `config.h` | board-dependent | `RELAY_ACTIVE_LEVEL` compile-time constant — set to match your relay board (many are ACTIVE-LOW), re-flash |
| Timezone | Display Unit | Asia/Manila | POSIX TZ string `PHT-8` (UTC+8, no DST); schedule times are local |
| NTP | Display Unit | pool servers | Synced periodically; UI shows synced time |

**Timezone note:** the firmware uses the POSIX form `PHT-8` — a positive UTC offset is written with a minus sign in POSIX. Schedule entries entered on the Schedule screen are interpreted in this timezone.

**Token note:** `catfish` is a default, not a feature. Change it before going live: `curl -X POST http://catfish-relay.local/config -H 'Content-Type: application/json' -d '{"token":"your-new-token"}'`. The display unit's Settings screen must be updated with the same token so `POST /feed` keeps working. Changing the token via `/config` takes effect immediately and survives reboots.

## OTA updates

The relay unit runs ArduinoOTA, so firmware updates need no cable once it is on the network:

```sh
pio run -d firmware/relay-unit -t upload --upload-port catfish-relay.local
```

Requirements: display unit/PC and relay unit on the same LAN, mDNS resolution working (`.local`), and the relay unit idle. Display Unit OTA is on the roadmap (see the README); for now flash it over USB.

## Partitions & PSRAM notes

- Flash: 16 MB QIO (`board_upload.flash_size = 16MB`, `board_build.flash_mode = qio`), partition table `default_16MB.csv` — large OTA-capable app slots, SPIFFS area, and the `boot_app0.bin` OTA-data sector at `0xe000` that selects the running slot.
- PSRAM: 8 MB octal on the N16R8 module — requires `board_build.arduino.memory_type = qio_opi` and `-DBOARD_HAS_PSRAM`. Wrong memory type is the usual cause of PSRAM init failures.
- LVGL pool: `include/lv_conf.h` sets `LV_MEM_SIZE` to 64 KB with the Montserrat fonts needed by the UI already enabled.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| White screen on the display unit | Panel not initialized (bad board config or boot loop before LVGL starts) | Check serial logs; verify `include/esp_panel_board_custom_conf.h` was not modified; re-flash all four images at the correct offsets |
| No touch response | GT911 reset/int sequence handled via CH422G expander | Confirm vendored libs are 1.0.0/1.0.1 (not registry variants); check logs for `GT911`/expander init errors; power-cycle fully |
| Relay not found by display unit | Different subnet/AP isolation, mDNS blocked, relay offline | Ping `catfish-relay.local`; test UDP discovery (see [api.md](api.md)); ensure both units are on the same 2.4 GHz SSID and that client isolation is off |
| Setup portal not appearing | Credentials already saved, or portal only shows when WiFi fails | Erase flash (`pio run -d <dir> -t erase`) and re-provision; check the phone actually joined the setup AP |
| `PSRAM init failed` at boot | Wrong memory type or missing build flag | Keep `qio_opi` and `-DBOARD_HAS_PSRAM` in `firmware/display-unit/platformio.ini`; verify you have the N16R8 module (8 MB PSRAM) |
| WiFi keeps dropping | Weak 2.4 GHz signal, PSU sag, RF congestion | Move closer to the AP, check the 5 V rail under motor load (brown-outs reset the NodeMCU), pick a clearer channel |
| Upload "Failed to connect" (S3) | Board not in download mode | Hold BOOT while connecting USB-C, then retry; use a data cable |
| Upload fails (NodeMCU) | CH340 driver missing / wrong port | Install the CH340 driver, pick the correct `<PORT>` (`pio device list`), close other serial monitors |
| Motor runs at boot / continuously | Relay board is ACTIVE-LOW with mismatched `RELAY_ACTIVE_LEVEL` | Flip `RELAY_ACTIVE_LEVEL` in `config.h`, re-flash; verify idle state before leaving unattended |
