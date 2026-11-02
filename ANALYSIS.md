# Catfish Feeder (smartfishfeeder) — Full Codebase Analysis

> Generated 2026-11 from a full read of the codebase: `README.md`, `CLAUDE.md`,
> all `docs/`, both `platformio.ini`, `.github/workflows/firmware.yml`,
> `scripts/`, all `firmware/common/*` headers, all 25 display-unit `src/`
> files, relay-unit firmware, all 6 native test suites + mocks, `web-flasher/`,
> `dashboard/index.html`, `.devcontainer/`. Vendored libs (`lib/`) verified for
> version/config only.
>
> **Status update (v1.10.1):** the three highest-priority findings — Q1
> (retry journal loses per-tank grams), Q3 (GPIO43/44 vs UART0 for the nRF24
> build) and S1 (open ArduinoOTA on both units) — were fixed and shipped in
> v1.10.1 (commits e8d0497, 5799dbd, 2d4b7f0). They are marked **FIXED
> v1.10.1** below; the remainder of the report is unchanged audit material.

## 1. Project overview

Self-hosted, two-board automatic catfish feeder: an ESP32-S3 touch "scheduler"
panel plus an ESP8266 relay that pulses a 5 V auger motor. No cloud; LAN-only,
optional MQTT/Home Assistant. Current version **1.10.1**
(`firmware/common/catfish_protocol.h:20`), wire-revision 1.
(v1.10.1 shipped the three priority fixes below; version bump in 12b7129.)

**Four subsystems:**

1. **Display Unit** (`firmware/display-unit/`) — Waveshare ESP32-S3-Touch-LCD-4.3B,
   LVGL 8.4 UI, and everything smart: scheduler, grams dosing, policy engine,
   retry journal, MQTT hub + HA discovery, web dashboard + OTA, WiFi manager,
   ESP-NOW/nRF24 receivers, RTC, SD log. It is the HTTP *commander*; up to 4
   relay "tanks" (`relay_client.h:38`).
2. **Relay Unit** (`firmware/relay-unit/`) — ESP8266 NodeMCU V3, deliberately
   dumb: HTTP server (`/health`, `/status`, `/feed`, `/config`), UDP discovery
   on :5050, mDNS `catfish-relay`, ArduinoOTA, EEPROM config, three feeding
   mechanisms (motor/servo/stepper) sharing the "feed for N seconds" wire
   contract (`firmware/common/catfish_mech.h:5-13`).
3. **Web flasher** (`web-flasher/`) — ESP Web Tools v10.4.0 page on GitHub
   Pages with a version picker mirroring release binaries to the Pages origin.
4. **Docs + AI tooling** (`docs/`, `CLAUDE.md`, `.mcp.json`, `.devcontainer/`).

**Data flow:** Touch/MQTT/web/ESP-NOW → `UiEvent` FreeRTOS queue
(`ui.cpp:2943`) → `loop()` `handleUiEvent()` (`main.cpp:727`) → policy gate
`feed_state::checkFeed()` (`feed_state.cpp:465`) → grams→seconds
`catfishGramsToSeconds()` (`catfish_protocol.h:84`) → `net_worker` task
(`net_worker.cpp:26`) → `relay_client::feedTank()` (`relay_client.cpp:347`) →
relay `startFeedPulse()` (relay `main.cpp:302`). Results return via a result
queue into `handleWorkerResult()` (`main.cpp:1030`) → history ring + SD CSV +
MQTT retained state + hopper estimator. Time: NTP→PCF85063 RTC→epoch-minute
fire markers (`feed_scheduler.cpp:279`).

## 2. Build system

- Display unit: pioarduino 55.03.312 (Arduino core 3.3.x) pinned by URL,
  `esp32-s3-devkitc-1`, `qio_opi`, 16 MB flash, `default_16MB.csv` partitions.
  Feature gates `CATFISH_FEATURE_SD=1`, `CATFISH_FEATURE_ESPNOW=1` (nRF24
  opt-in), tunable `CATFISH_LCD_PCLK_HZ`. lib_deps: lvgl@8.4.0,
  ArduinoJson@7.4.2, WiFiManager@2.0.17, espMqttClient@1.7.3, RF24@1.4.8.
  Envs: `display-unit`, `display-unit-qemu` (`-DCATFISH_QEMU=1`), `native`
  (Unity host tests that `#include` the real `.cpp` files).
- Pre-script `scripts/embed_dashboard.py` regenerates `src/dashboard_html.h`
  (gzip level 9, `mtime=0` reproducible).
- Relay unit: espressif8266@4.2.1, `nodemcuv2`. No tests, no QEMU (ESP8266
  unsupported by Espressif QEMU).
- CI (`.github/workflows/firmware.yml`, 6 jobs): native tests, both builds
  (tag↔`CATFISH_APP_VERSION` guard), headless QEMU smoke (pinned Espressif
  QEMU `esp-develop-9.2.2-20260417`, verdict greps in `scripts/qemu_run.sh`),
  Pages deploy with release mirroring + `versions.json`, two release channels
  (stable `v*` tags + rolling `latest` prerelease).

## 3. Hardware layer — board fit (Waveshare ESP32-S3-Touch-LCD-4.3B)

**Excellent.** `include/esp_panel_board_custom_conf.h` matches the verified
notes in `docs/notes/hardware-4.3b.md` line-for-line:

| Peripheral | Config | Evidence |
|---|---|---|
| LCD | ST7262, RGB565, 800x480, no 3-wire SPI control | `esp_panel_board_custom_conf.h:68-73` |
| RGB pins | HSYNC 46, VSYNC 3, DE 5, PCLK 7; D0..D15 = 14,38,18,17,10,39,0,45,48,47,21,1,2,42,41,40 | `:134-163` |
| Timing | PCLK 16 MHz (build-tunable), falling-edge active, H 4/8/8, V 4/16/16 | `:104-120` |
| Touch | GT911, I2C host 0, SDA 8/SCL 9 @ 400 kHz, INT GPIO4, addr 0x5D/0x14 | `:208, 251-262` |
| IO expander | CH422G @ 0x20; LCD_RST EXIO3, TP_RST EXIO1, backlight DISP EXIO2 (**switch, no PWM**) | `:307, 316-317, 344-431` |
| I2C ownership | Panel stack owns the **legacy** `driver/i2c.h` on host 0; Arduino `Wire` banned (v1.8.1 black-screen war story) | `:228-239, 356` |
| RTC | PCF85063 @ 0x51, same legacy I2C API, probed after `Board::begin()` | `pcf85063.h:39-41`, `main.cpp:1499-1504` |
| TF card | SPI 11/12/13, CS via CH422G EXIO4, shared bus with nRF24 via `spi_bus_guard` mutex | `sd_log.cpp:25-53` |
| Framebuffers | 2 × PSRAM buffers, avoid-tearing mode 3 (double buffer + direct mode) | `main.cpp:1462-1470` |

**Deviation risk (resolved v1.10.1):** nRF24L01 uses GPIO43/44 (CE/CSN) — the
4.3B's RS485 pins and the ESP32-S3 **UART0 defaults**; the main env now routes
Serial to the native USB CDC (`ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1`,
5799dbd), leaving 43/44 free for the radio (Q3).

## 4. Tasks & concurrency

LVGL task (core 1, prio 2), wifi-task (core 0, prio 3, 16 KB), net-worker
(core 0, prio 2, 12 KB), web-task (core 0, prio 1), espMqttClient task,
optional rf-task. `loop()` never blocks: drains 4 queues every ~50 ms, then
1 Hz/30 s/10 s/60 s cadence blocks (`main.cpp:1562-1762`).

Key modules: feed_scheduler (8 entries, NVS JSON, epoch/60 fire markers,
weekday mask, atomic `replaceAll`), feed_state (retry journal, 2 slots,
30 s/5 min/15 min backoff, fired-map catch-up, policy guards, hopper
estimator, 24-entry history ring), net_wifi (WiFiManager + on-display scan/
connect), prefs (RAM cache + write-through NVS, spinlocks), mqtt_mgr
(retained-command scrubbing on reconnect, LWT, 20+ HA discovery entities),
web_server (gzip dashboard, token-gated PUT/POST with constant-time compare,
OTA gated before first flash byte), net_espnow/net_rf24 (12-byte datagram,
FNV-1a key, seq dedup ring), sd_log (CSV, 2 MB rotation, speed ladder).

## 5. Code quality findings

| # | Severity | Finding | Evidence |
|---|---|---|---|
| Q1 | **Medium** — **FIXED v1.10.1** | Per-tank gram overrides are lost across scheduled-feed retries (`RetrySlot` stores only shared grams; retries pass `gramTank=nullptr`). Fixed in e8d0497: `gramTank[4]` on `RetrySlot`/`RetryDue` + `"gt"` persistence + tests | `feed_state.cpp:26-32`, `main.cpp:1674-1680` |
| Q2 | **Medium** | Multi-tank history/counter fidelity: "last applied wins" seconds recorded for the whole feed; per-tank `secOk[]` not reflected in `HistEntry` | `net_worker.cpp:60-73`, `feed_state.h:54-61` |
| Q3 | **Medium** — **FIXED v1.10.1** | GPIO43/44 conflict risk (nRF24 vs UART0 `Serial`). Fixed in 5799dbd: USB-CDC build flags on the main env only (QEMU env deliberately keeps UART0 for the smoke-test verdict markers) | `net_rf24.h:10-14, 39-44`, `platformio.ini:30-44` |
| Q4 | Medium-low | Non-atomic ~2.4 KB Snapshot publish vs concurrent web serialization (formal data race, torn tank arrays possible) | `web_server.cpp:337, 35, 75-118` |
| Q5 | Low | Unlocked reads of shared char IPs in worker HTTP paths (worst case: one failed request, self-heals) | `relay_client.cpp:26-42, 297-309` |
| Q6 | Low | Cross-task char-buffer races in net_wifi (bounded buffers → garbled snapshot at worst) | `net_wifi.cpp:97-125, 245-263` |
| Q7 | Low | Duplicated RF transport logic (seqSeen/key derivation) in net_espnow and net_rf24 | both files |
| Q8 | Low | Inconsistent "never fired" sentinels (0xFFFF vs 0) | `feed_scheduler.cpp:28-30, 143` |
| Q9 | Low | Fragile `ti += snprintf(...)` accumulation pattern in history formatter | `ui.cpp:3561-3567` |
| Q10 | Low | Stale doc comments after v1.10.0 grams migration | `prefs.h:123`, `ui.h:219-224`, `ui.cpp:140` |
| Q11 | Low | Board-begin failure halts forever with white screen, no reboot | `main.cpp:1471-1474` |
| Q12 | Low | OTA error conflation: stale `Update.hasError()` turns a fresh unauth request into `500` instead of `403` | `web_server.cpp:272-282` |
| Q13 | Info | Documented magic numbers (named/commented) — acceptable | — |
| Q14 | Info | Contract drift: header says recv-cb does "no prefs reads", it does one | `net_espnow.h:6-7`, `net_espnow.cpp:77` |

**Strong points:** strict single-writer discipline via event queues;
retained-command scrubbing against MQTT replay; centrally clamped protocol
helpers shared by firmware AND tests; constant-time token compares on both
ends; relay boot-safety latch pre-load; validate-then-mutate config handling;
bounded `strlcpy` everywhere (no `strcpy`/`sprintf` in project code); static
asserts on EEPROM layouts.

## 6. Security findings

| # | Severity | Finding | Evidence |
|---|---|---|---|
| S1 | **Medium** — **FIXED v1.10.1** | Open ArduinoOTA by default on both units. Fixed in 2d4b7f0: random 10-char password generated on first boot (display: NVS via prefs; relay: additive EEPROM record @128), surfaced in Settings > OTA UPDATE / token-gated POST /config, explicit `otaOpen` opt-out, build-time `-DOTA_PASSWORD` still wins | `main.cpp:52-57, 552-557`; relay `config.h:77-83` |
| S2 | **Medium** | Web-upload OTA has token auth but no image signature verification — token possession == firmware implant | `web_server.cpp:237-270` |
| S3 | **Medium** | Default pairing token `catfish` ships in header/EEPROM/dashboard pre-fill; only real control for destructive actions | `catfish_protocol.h:52-56`; `dashboard/index.html:160` |
| S4 | Medium-low | ESP-NOW/nRF24 "auth" = 32-bit FNV-1a(token), no MAC/encryption, sender MAC ignored, 8-entry replay window | `catfish_rfnode.h:52-61`, `net_espnow.cpp:29-66` |
| S5 | Low | Unauthenticated GET APIs leak state (tank IPs, history, stats) | `web_server.cpp:290-299` |
| S6 | Low | MQTT plaintext (1883, no TLS), creds unencrypted in NVS | `mqtt_mgr.cpp:215-221` |
| S7 | Low | Token in URL query on relay `GET /feed?token=T` (lands in HTTP logs) | relay `main.cpp:583-603` |
| S8 | Low | CI actions pinned by major tag, not commit SHA | `firmware.yml` throughout |
| S9 | Info | Web flasher loads unpkg esp-web-tools without SRI | `web-flasher/index.html:8-10` |
| S10 | Info | Positive: constant-time compares, custom-header CSRF resistance, MQTT scrubbing, OTA token before first byte, no hardcoded secrets anywhere | — |

## 7. Tests & CI

Native suite: 6 Unity suites (~2,050 LOC) covering scheduler, feed_state,
RF wire format, time math, MQTT payloads, mech config — with injectable
clocks. QEMU smoke: full boot on 16 MB emulated flash, LVGL tree in RAM
framebuffer, injected wall clock, real event pipeline (schedule add, WiFi-down
feed abort accounting, vacation, skip-next), crash-marker/boot-loop greps.

**Gaps:** relay unit has zero tests (safety-critical clamping lives there);
QEMU can't exercise panel/touch/RTC/SD/WiFi/HTTP/MQTT/OTA; no net_worker/
relay_client/web_server handler tests; no static analysis (cppcheck/
clang-tidy), no `-Wall -Wextra`; release-mirror `pick()` Python untested.

## 8. Prioritized suggestions

1. ~~**Persist per-tank grams in the retry journal**~~ — **DONE v1.10.1**
   (e8d0497).
2. ~~**Fix GPIO43/44 vs UART0**~~ — **DONE v1.10.1** (5799dbd).
3. **Make the web Snapshot race-free** — mutex/double-buffer/seqlock in
   `web_server.cpp:337`. *Effort S–M.*
4. **Record per-tank outcomes in history/SD** — extend `HistEntry` with
   `secOk[4]`, feed from `Result::secOk`. *Effort S.*
5. **Harden the RF feed path** — HMAC-SHA256 key or ESP-NOW LMK + MAC
   allowlist + monotonic per-node replay window. *Effort M.*
6. **Deduplicate the RF transports** — move `seqSeen`/key derivation into
   `firmware/common/catfish_rfnode.h`. *Effort S.*
7. ~~**Default-on OTA password**~~ — **DONE v1.10.1** (2d4b7f0, both units,
   with EEPROM/NVS persistence + settings surface).
8. **Add relay-unit native env + tests** — extract `RelayConfig` validation
   into a header-only pure helper; add `pio test -e native` to CI. *Effort M.*
9. **Extend the QEMU smoke test** — scheduled-feed firing + journal-retry +
   catch-up cycle end-to-end with the injectable clock; soft-reboot case.
   *Effort M.*
10. **CI hardening** — pin actions by full SHA, add `-Wall -Wextra` (ideally
    `-Werror`) pass, run cppcheck/clang-tidy, extract + unit-test the release
    mirror `pick()` Python. *Effort S–M.*
11. **LVGL memory strategy** — consider `LV_MEM_CUSTOM=1` +
    `heap_caps_malloc(MALLOC_CAP_SPIRAM)` or at least `LV_MEM_MONITOR` so UI
    growth can't wedge the 160 KB static pool again. *Effort S.*
12. **Signal board-begin failure on-screen** — minimal LVGL error screen +
    `ESP.restart()` loop instead of silent white halt. *Effort S.*
13. **Docs/comment sync for the grams migration** + one-page threat-model
    table consolidating the "LAN-only by design" decisions (S3–S6). *Effort S.*

## Bottom line

An unusually disciplined hobby-grade codebase: verified pin-level board
bring-up for the 4.3B, strict task/lock discipline, centrally clamped protocol
helpers shared by firmware and tests, a real headless QEMU CI gate, and a
history of fixing its own audit findings (v1.8.1–v1.8.4, now v1.10.1: Q1, Q3
and S1 from this report). Residual risks are concentrated in (a) security
resting on a default LAN token + unsigned web OTA (S2–S4), (b) the remaining
multi-tank data-fidelity regression (Q2), and (c) zero coverage for the
ESP8266 relay half. All addressable with the remaining S/M-effort items above.
