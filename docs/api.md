# Relay Unit API Reference

The relay unit (ESP8266 NodeMCU V3) exposes a small HTTP API on port 80, mDNS, and a UDP discovery protocol on port 5050. The **Display Unit is the primary client** — it discovers the relay and sends feed commands — but everything below is plain HTTP/UDP, so you can script it from any device on your LAN.

Design goals: LAN-only, zero cloud dependencies, minimal auth. For the wiring and hardware side see [hardware.md](hardware.md); flashing/OTA in [firmware.md](firmware.md).

## Base URL and naming

| Name | Value |
|------|-------|
| mDNS hostname | `catfish-relay.local` (HTTP port 80) |
| UDP discovery port | `5050` |
| Feed token | sent **inside the JSON body** (or `?token=` on the GET variant), default **`catfish`** |
| OTA service | ArduinoOTA (upload via PlatformIO, see [firmware.md](firmware.md)) |

Requests that change state (`POST /feed`, `POST /config`) require the token. Read-only endpoints (`GET /status`, `GET /health`) do not.

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
| `seconds` | integer | yes | Motor run time **in seconds**. Must be a strict JSON integer (`3` ok, `"3"`/`3.5` → 400). Hard cap 60 s; values above the configured `maxPulseS` (default 30) are clamped down, not rejected. |
| `token` | string | yes | Current auth token |

A `GET /feed?seconds=N&token=T` variant exists with identical semantics (convenient for curl/browser testing).

Order of checks (deterministic error codes):

| Status | Meaning | Body |
|--------|---------|------|
| 200 | Feed pulse started | `{"ok":true,"seconds":3}` (plus a note field if the value was clamped to `maxPulseS`) |
| 400 | Malformed JSON, or `seconds` missing/not an integer/`< 1` | `{"ok":false,"error":"bad_json"}` or `{"ok":false,"error":"bad_seconds"}` |
| 403 | Wrong or missing token | `{"ok":false,"error":"auth"}` |
| 409 | A feed is already running (busy) | `{"ok":false,"error":"busy"}` |

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
  "uptimeS": 86400,
  "heap": 21340,
  "rssi": -58,
  "ip": "192.168.1.42",
  "version": "1.0.0",
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
| `uptimeS` | int | Device uptime in seconds |
| `heap` | int | Free heap bytes |
| `rssi` | int | WiFi signal in dBm |
| `ip` | string | Current IP address |
| `version` | string | Firmware version |
| `ntpSynced` | bool | `true` once NTP time is valid (affects `lastFeedEpoch` accuracy) |

```sh
curl http://catfish-relay.local/status
```

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

| Status | Meaning |
|--------|---------|
| 200 | Config updated — response echoes the applied config |
| 400 | Malformed JSON, out-of-range values, or token too long |
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
```

Settings persist to EEPROM (survive reboots and power cuts).

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
3. **UDP discovery** — broadcast `CATFISH_DISCOVER` on port 5050 and take the first `CATFISH_RELAY|<ip>` answer (1.5 s window).
4. **Fail** — the UI shows "Relay unreachable"; discovery retries on the next probe cycle or feed attempt.

Successful IPs are persisted, so a relay that changes IP (DHCP) self-heals within a minute or two.

## Security notes

- **LAN-only by design.** There is no cloud relay, no inbound port mapping, no TLS (an ESP8266 serving TLS is out of scope here). Do not port-forward the relay unit to the internet.
- The token guards `POST /feed` and `POST /config`; `/status`, `/health` and `/` are readable by anyone on the LAN. On a home network this is acceptable; on shared networks it is not.
- **Change the default token (`catfish`)** before real use via `POST /config` (`newToken`) — the display unit reads the token from its own NVS settings; keep both sides in sync.
- Because HTTP is plaintext, treat the token as a light fence, not a secret: anyone sniffing the LAN can read it. Physically securing the network is the real control.
- Planned hardening (see README roadmap): MQTT with credentials for Home Assistant integration.
