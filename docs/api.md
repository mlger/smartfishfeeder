# Relay Unit API Reference

The relay unit (ESP8266 NodeMCU V3) exposes a small HTTP API on port 80, mDNS, and a UDP discovery protocol on port 5050. The **Display Unit is the primary client** — it discovers the relay and sends feed commands — but everything below is plain HTTP/UDP, so you can script it from any device on your LAN.

Design goals: LAN-only, zero cloud dependencies, minimal auth. For the wiring and hardware side see [hardware.md](hardware.md); flashing/OTA in [firmware.md](firmware.md).

## Base URL and naming

| Name | Value |
|------|-------|
| mDNS hostname | `catfish-relay.local` (HTTP port 80) |
| UDP discovery port | `5050` |
| Feed token | sent **inside the JSON body** (or `?token=` on the GET variant), default **`catfish`** |
| OTA service | ArduinoOTA (upload via PlatformIO, see [firmware.md](firmware.md)); password optional via build flag |

Requests that change state (`POST /feed`, `POST /config`) require the token. Read-only endpoints (`GET /status`, `GET /health`) do not.

The **Display Unit's Settings screen manages the token end-to-end**: Settings → Relay Unit → *Pairing token* opens an on-screen editor. Saving updates the display's copy; ticking **"Also update the relay unit"** additionally rotates the relay's token via `POST /config` (this requires the display's current token to still match the relay, since it authorizes the rotation).

```sh
curl http://catfish-relay.local/health
```

If mDNS does not resolve on your machine, use the relay's IP from discovery (below) or your router's client list.

## POST /feed

Energizes the relay for a pulse, running the feeder motor.

- **Auth:** required — the current token in the JSON body
- **Body:** JSON

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `seconds` | integer | yes | Feed duration **in seconds** (see *Feeding mechanisms* below). Must be a strict JSON integer (`3` ok, `"3"`/`3.5` → 400). Hard cap 60 s; values above the configured `maxPulseS` (default 30) are clamped down, not rejected. |
| `token` | string | yes | Current auth token |

A `GET /feed?seconds=N&token=T` variant exists with identical semantics (convenient for curl/browser testing).

Order of checks (deterministic error codes):

| Status | Meaning | Body |
|--------|---------|------|
| 200 | Feed pulse started | `{"ok":true,"seconds":3}` — plus a `note` string when the value was clamped to `maxPulseS` |
| 400 | Malformed JSON, or `seconds` missing/not an integer/`< 1` | `{"ok":false,"error":"bad_json"}` or `{"ok":false,"error":"bad_seconds"}` |
| 403 | Wrong or missing token | `{"ok":false,"error":"auth"}` |
| 409 | A feed is already running (busy) | `{"ok":false,"error":"busy"}` |

The `seconds` field in a 200 response is what the relay **actually applied** (after clamping). Display-side toasts surface the clamp, e.g. `Fed OK (30 s) - clamped`.

Examples:

```sh
# explicit 3 s pulse
curl -X POST http://catfish-relay.local/feed \
     -H "Content-Type: application/json" \
     -d '{"seconds":3,"token":"catfish"}'

# GET convenience variant
curl "http://catfish-relay.local/feed?seconds=3&token=catfish"
```

Out-of-range values are clamped, not rejected: `seconds: 120` runs for 60 s (hard cap), and `seconds: 45` with `maxPulseS: 30` runs for 30 s with a clamp note in the response.

## GET /status

Full runtime snapshot (no auth).

```json
{
  "ok": true,
  "state": "ready",
  "feeding": false,
  "secondsLeft": 0,
  "lastFeedEpoch": 1760000000,
  "pulseS": 3,
  "maxPulseS": 30,
  "mechanism": "motor",
  "servoRestDeg": 0,
  "servoFeedDeg": 90,
  "stepHz": 400,
  "uptimeS": 86400,
  "heap": 21340,
  "rssi": -58,
  "ip": "192.168.1.42",
  "version": "1.4.0",
  "proto": 1,
  "ntpSynced": true
}
```

| Field | Type | Description |
|-------|------|-------------|
| `ok` | bool | Always `true` |
| `state` | string | `"ready"` or `"feeding"` |
| `feeding` | bool | `true` while the relay contact is closed (motor energized) |
| `secondsLeft` | int | Seconds remaining in the current pulse (0 when idle) |
| `lastFeedEpoch` | int | Unix epoch of the last feed pulse. Millis-based pseudo-epoch until first NTP sync — check `ntpSynced` |
| `pulseS` | int | Default pulse duration (used by the display unit's Feed Now) |
| `maxPulseS` | int | Configured clamp for feed requests (1-60) |
| `mechanism` | string | Feeding mechanism: `"motor"` (default), `"servo"` or `"stepper"` (v1.4.0+) |
| `servoRestDeg` | int | Servo idle angle, 0-180 (meaningful when `mechanism` is `servo`) |
| `servoFeedDeg` | int | Servo feed angle, 0-180 |
| `stepHz` | int | Stepper step rate, steps/s, 1-1000 (meaningful when `mechanism` is `stepper`) |
| `uptimeS` | int | Device uptime in seconds |
| `heap` | int | Free heap bytes |
| `rssi` | int | WiFi signal in dBm |
| `ip` | string | Current IP address |
| `version` | string | Firmware version |
| `proto` | int | Wire-contract revision (see `CATFISH_PROTOCOL_VERSION` in `firmware/common/catfish_protocol.h`). The display unit compares it to its own value and warns on a mismatch — the usual cause is only one unit being re-flashed |
| `ntpSynced` | bool | `true` once NTP time is valid (affects `lastFeedEpoch` accuracy) |

```sh
curl http://catfish-relay.local/status
```

### Protocol versioning

Firmware on both units must agree on the wire contract (endpoints, JSON fields, discovery strings). Each build embeds `CATFISH_PROTOCOL_VERSION`; the relay reports it as `"proto"` in `/status`, and the display logs a warning (once per boot) and marks the *Test connection* toast with **(protocol mismatch!)** when the values differ. If you see it, re-flash both units from the same commit/tag.

## GET /health

Lightweight liveness probe — plain-text `OK` with HTTP 200, no JSON, no auth. The display unit uses it to validate discovered IPs. Use `/status` when you need detail.

```sh
curl http://catfish-relay.local/health
# OK
```

## GET /

Serves a minimal built-in HTML status page (auto-refreshes every 10 s). Handy as a smoke test from a phone browser without any tooling — but for daily use the display unit's UI is the intended front-end.

## POST /config

Changes device configuration. Requires the **current** token for auth; an optional `newToken` rotates the credential.

- **Auth:** required (`token` field = current credential)
- **Body:** JSON

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `token` | string | yes | Current token (used for auth) |
| `newToken` | string | no | Replacement token (non-empty, shorter than 24 chars). Takes effect immediately and persists |
| `maxPulseS` | integer | no | New clamp for feed requests, 1-60 |
| `pulseS` | integer | no | New default pulse duration, 1-30 |
| `mechanism` | string | no | `"motor"`, `"servo"` or `"stepper"` (v1.4.0+). An unknown name is rejected with 400 `bad_mechanism` before anything else is applied |
| `servoRestDeg` | integer | no | Servo idle angle, clamped to 0-180 |
| `servoFeedDeg` | integer | no | Servo feed angle, clamped to 0-180 |
| `stepHz` | integer | no | Stepper step rate, clamped to 1-1000 steps/s |
| `stepDir` | integer | no | Stepper DIR level during feed, 0 or 1 |

| Status | Meaning |
|--------|---------|
| 200 | Config updated — response echoes the applied config (including the mechanism block) |
| 400 | Malformed JSON, out-of-range values, unknown `mechanism`, or token too long |
| 403 | Wrong current token |

```sh
# rotate the token (recommended before real use)
curl -X POST http://catfish-relay.local/config \
     -H "Content-Type: application/json" \
     -d '{"token":"catfish","newToken":"tuna-2025"}'

# tighten the max pulse to 10 s
curl -X POST http://catfish-relay.local/config \
     -H "Content-Type: application/json" \
     -d '{"token":"tuna-2025","maxPulseS":10}'

# switch to a servo sweeping 20 deg -> 110 deg
curl -X POST http://catfish-relay.local/config \
     -H "Content-Type: application/json" \
     -d '{"token":"tuna-2025","mechanism":"servo","servoRestDeg":20,"servoFeedDeg":110}'

# switch to a stepper at 500 steps/s, DIR high while feeding
curl -X POST http://catfish-relay.local/config \
     -H "Content-Type: application/json" \
     -d '{"token":"tuna-2025","mechanism":"stepper","stepHz":500,"stepDir":1}'
```

Settings persist to EEPROM (survive reboots and power cuts). The mechanism lives in a **separate additive EEPROM record** (own magic + checksum at offset 64), so firmware upgrades never disturb the original token/pulse blob — first boot after upgrading just writes mechanism defaults (`motor`). The display unit's Settings → Relay Unit → **Mechanism → Configure** edits all of this over the same endpoint; the factory reset (hold FLASH 5 s) resets the mechanism too.

### Feeding mechanisms (v1.4.0+)

`POST /feed` semantics are identical for every mechanism: **"feed for N seconds"** — the relay interprets those seconds differently per mechanism:

| Mechanism | Pins | During the pulse |
|-----------|------|------------------|
| `motor` (default) | D1 relay/FET (existing wiring) | Output ON for N seconds |
| `servo` | D5 signal (+5V, GND) | Sweeps rest angle ↔ feed angle every 0.5 s; parks at rest and detaches afterwards |
| `stepper` | D2 STEP, D6 DIR (A4988/DRV8825) | STEP pulses at `stepHz`; DIR held at the `stepDir` level |

## UDP discovery protocol

The relay unit listens on **UDP port 5050** and answers discovery requests so the display unit (and your scripts) can find it without knowing its IP.

- Send the ASCII string `CATFISH_DISCOVER` as a UDP broadcast to port `5050`.
- The relay replies to the sender with: `CATFISH_RELAY|<ip>` (e.g. `CATFISH_RELAY|192.168.1.42`).

Test from a Linux/macOS machine:

```sh
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.sendto(b"CATFISH_DISCOVER", ("255.255.255.255", 5050))
print(s.recvfrom(256))
PY
```

If nothing answers: client isolation on the AP, a different subnet/VLAN, or the relay unit is offline.

## How the display unit finds the relay

The display unit resolves the relay in this order:

1. **Cached last-known IP** — validated with `GET /health` (2 s timeout).
2. **mDNS** — look up `catfish-relay.local`.
3. **UDP discovery** — broadcast `CATFISH_DISCOVER` to both `255.255.255.255:5050` **and the subnet-directed broadcast address** (works around APs that filter global broadcasts), then take the first `CATFISH_RELAY|<ip>` answer (1.5 s window).
4. **Fail** — the UI shows "Relay unreachable"; discovery retries on the next probe cycle or feed attempt.

**Stale-IP self-heal:** a persisted IP that stops answering is not trusted forever. Three consecutive transport-level failures (timeouts/refusals — any HTTP response means the relay is alive and resets the counter) drop the cached IP, and a failed feed POST immediately re-discovers and retries **once** against the fresh IP. A relay that changes IP (DHCP) therefore self-heals within a couple of minutes without a reboot.

## Security notes

- **LAN-only by design.** There is no cloud relay, no inbound port mapping, no TLS (an ESP8266 serving TLS is out of scope here). Do not port-forward the relay unit to the internet.
- The token guards `POST /feed` and `POST /config`; `/status`, `/health` and `/` are readable by anyone on the LAN. On a home network this is acceptable; on shared networks it is not.
- **Change the default token (`catfish`)** before real use — no `curl` needed: the display unit's Settings → Relay Unit → *Pairing token* editor can rotate the relay's token directly (tick "Also update the relay unit"). Both sides stay in sync because the display only persists the new token after the relay accepted it.
- Because HTTP is plaintext, treat the token as a light fence, not a secret: anyone sniffing the LAN can read it. Physically securing the network is the real control.
- Planned hardening (see README roadmap): MQTT with credentials for Home Assistant integration.
