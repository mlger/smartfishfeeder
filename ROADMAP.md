# Roadmap

Living plan for the Catfish Feeder project. The engineering findings behind
items marked **[audit]** live in [ANALYSIS.md](ANALYSIS.md) (code-quality Q#,
security S#, prioritized suggestions); shipped features are logged in
[docs/firmware.md](docs/firmware.md) release sections.

Status legend: ✅ shipped · 🔜 planned · 💤 deferred

## Shipped highlights

| Version | Theme |
|---|---|
| v1.1.0 | Display-unit OTA + protocol version handshake |
| v1.2.0 | MQTT hub + Home Assistant auto-discovery |
| v1.3.0 | Built-in web dashboard (display unit) |
| v1.4.0 | Servo / stepper feeding mechanisms |
| v1.5.0 | Multi-tank / multi-relay (cap 4) |
| v1.6.0 | PCF85063 RTC + 12/24h clock |
| v1.7.0 | microSD feed history with rotation |
| v1.8.0 | ESP-NOW / nRF24 wireless feed buttons |
| v1.9.0 | Feeding feature pack (weekday masks, per-tank portions, catch-up, vacation, retry journal, hopper estimator, policy guards) |
| v1.10.0 | Grams dosing, on-display WiFi manager |
| v1.11.x | UI overhaul (hold-to-feed, undo, 7-day chart), on-device schedule editing + calibration |
| v1.12.0 | Sub-second millisecond relay pulses (calibration accuracy), self-hosted ESP Web Tools |

## v1.12.x — stabilization (patch train) 🔜

- [ ] Make the web Snapshot race-free — mutex/double-buffer/seqlock around the
      ~2.4 KB snapshot publish **[audit Q4 / suggestion #3]** (effort S–M)
- [ ] OTA status conflation: stale `Update.hasError()` turns a fresh
      unauthenticated request into `500` instead of `403` **[audit Q12]**
      (effort S)
- [ ] On-screen board-begin failure — minimal LVGL error screen + reboot loop
      instead of a silent white halt **[audit Q11 / suggestion #12]** (effort S)
- [x] Docs drift sweep — api.md stale "planned MQTT", hardware.md non-existent
      `duration_ms` field, LVGL pool size, ui.h fallback version string

## v1.13.0 — hardening 🔜

- [x] **Direct puck feed** — relay-side ESP-NOW listener (v1.13.0): puck
      presses feed with the router completely OFF. Shared, host-tested dedup
      machine (`catfish_rfdedup.h`, 12 test cases): the hub echoes the puck
      datagram `seq` in `/feed`; any authenticated feed cancels the relay's
      pending direct intent; a missed echo past a 1.5 s grace window feeds
      directly. Opt-in per relay (Settings → tank editor switch or POST
      /config `espnowDirect`); relay pins the last AP channel while the link
      is down and boots headless without a router.
- [ ] Multi-tank history fidelity: record per-tank `secOk[]` outcomes in
      history/SD instead of "last applied wins" **[audit Q2 / suggestion #4]**
      (effort S)
- [ ] Relay-unit native tests — extract `RelayConfig` validation into a
      header-only pure helper, add `pio test -e native` to CI **[suggestion #8]**
      (effort M)
- [ ] Harden the RF feed path — ESP-NOW LMK or HMAC-SHA256 key + MAC allowlist
      + monotonic per-node replay window **[audit S4 / suggestion #5]** (effort M)
- [ ] Deduplicate RF transports — move `seqSeen`/key derivation into
      `firmware/common/catfish_rfnode.h` **[suggestion #6]** (effort S)
- [ ] CI hardening — pin actions by full SHA, `-Wall -Wextra` (ideally
      `-Werror`), cppcheck/clang-tidy, test the release-mirror `pick()` Python
      **[audit S8 / suggestion #10]** (effort S–M)
- [ ] LVGL memory strategy — `LV_MEM_CUSTOM=1` + SPIRAM-backed pool or at
      least `LV_MEM_MONITOR` so UI growth can't wedge the 160 KB static pool
      again **[suggestion #11]** (effort S)

## v1.13.1 — patch 🔜

- [x] Honest "scan with router off" toast — *Scan for relays* is a UDP
      broadcast on the WiFi LAN and cannot work over ESP-NOW, so with the
      router off it reported a bare `Scan failed: WiFi down` (user-reported
      confusion right after the ESP-NOW offline work). When any relay has
      direct puck feed enabled, the toast now reads
      `No WiFi - cannot scan, pucks still feed directly`; otherwise the
      generic failure text stands. Gated on saved tank prefs (NVS) OR the
      relay's last-reported capability, so it survives a router-off reboot.
      QEMU smoke gains a scan-WiFi-down step proving the result path completes
      and the scan button un-wedges.

## v1.13.2 — patch 🔜

- [x] The v1.13.1 scan fix compared the wrong thing — `r.error` is a `char[]`
      (filled via `strlcpy`), so both `r.error == "WiFi down"` checks in the
      RES_SCAN handler compared **pointer addresses** (always false): the
      honest `No WiFi - cannot scan, pucks still feed directly` toast never
      fired and the new QEMU smoke scan-abort counter never incremented —
      the first-ever red run of that job caught it. Both checks now use
      `strcmp` content compare; QEMU smoke is green again (13/13 checks +
      SMOKE PASS verified locally on the emulated ESP32-S3).

## v1.14.0 — sensing 🔜

- [ ] Food-level sensor on the relay unit (D2) — wire into the existing hopper
      estimator: calibration, low-food alerts, MQTT state. Oldest deferred
      item (deliberately skipped in v1.9.0; see docs/firmware.md)
- [ ] Docs/comment sync + one-page threat model consolidating the
      "LAN-only by design" decisions (default token, plaintext LAN traffic,
      unauthenticated GETs) **[audit S3–S7 / suggestion #13]** (effort S)

## v1.15.0 — camera snapshot 🔜

- [ ] Camera snapshot on each feed event (last open item from the original
      README roadmap). Needs hardware selection first; the 4.3B display unit
      has free USB / RS485 / CAN pins (see docs/notes/hardware-4.3b.md).
      Snapshot → SD `/snaps/` + MQTT publish + dashboard gallery.

## Deferred / continuous 💤

- MQTT over TLS — display-unit side feasible, ESP8266 relay side out of
  scope; revisit only if the threat model changes
- QEMU smoke depth: journal-retry + catch-up cycle end-to-end, soft-reboot
  case **[suggestion #9]**
- Release notes with feature narrative per version (currently auto-generated
  flash instructions)
