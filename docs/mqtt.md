# MQTT & Home Assistant

The **display unit is the MQTT hub**: it publishes feeder state to your broker and accepts commands from Home Assistant (or anything else that speaks MQTT). The relay unit deliberately stays HTTP-only — the ESP8266 keeps its lean firmware and the display (16 MB flash, 8 MB PSRAM) carries the integration.

Configure everything on the device: **Settings → HOME ASSISTANT (MQTT) → Configure** — broker host/IP, port, username/password (optional), topic prefix (default `catfish`), and the *Connect to broker* switch. Settings persist across reboots; the card's status line shows `disabled / connecting… / connected / offline - retrying`.

## Topics

All state topics are **retained**. The single availability topic uses a retained **LWT** ("offline"), so Home Assistant marks the device unavailable whenever the display unit drops off the broker.

| Topic | Direction | Payload |
|-------|-----------|---------|
| `catfish/availability` | pub | `online` / `offline` (retained, LWT) |
| `catfish/last_feed` | pub | ISO-8601 UTC timestamp (HA `timestamp` sensor), `unknown` before the first feed |
| `catfish/next_feed` | pub | `"HH:MM"` or `none` |
| `catfish/feeding` | pub | `ON` while the motor pulse runs, `OFF` otherwise |
| `catfish/relay/online` | pub | `ON` when the relay unit answers `/status`, else `OFF` |
| `catfish/pulse/state` | pub | Default pulse seconds (1-30) |
| `catfish/feed/cmd` | sub | `ON` → feed now with the default pulse |
| `catfish/pulse/set` | sub | `1`…`30` → change the default pulse (clamped) |

Replace `catfish` with your configured prefix. Discovery configs are published (retained) to the standard Home Assistant prefix, e.g. `homeassistant/button/catfish_feed_now/config`.

## Home Assistant entities

| Entity | Type | Notes |
|--------|------|-------|
| Feed now | `button` | Triggers a real feed through the same worker pipeline as the touchscreen |
| Default pulse | `number` (1-30 s) | Two-way synced with the Settings spinbox |
| Relay online | `binary_sensor` (`connectivity`) | Mirrors the relay probe result |
| Feeding | `binary_sensor` | `on` for pulse + 2 s after each feed |
| Last feed | `sensor` (`timestamp`) | ISO-8601, usable in HA automations/dashboards |
| Next feed | `sensor` | `"HH:MM"` string |

After entering the broker settings, HA auto-discovers everything within a few seconds (Settings → Devices & services). The device appears as **Catfish Feeder** with the firmware version as `sw_version`.

## Behaviour notes

- **Commands are queued, never blocking**: MQTT callbacks run on the async-TCP task and only enqueue UI events; `loop()` applies them (same single-writer path as touchscreen input). A `feed/cmd` while a feed is already running returns the relay's 409-busy and the display toasts it — check the `Feeding` binary sensor in your automation if you script bursts.
- **Reconnect/backoff**: if the broker is unreachable the display retries with exponential backoff (5 s → 60 s cap) and publishes discovery + a full state snapshot after every successful reconnect.
- **Honest limitation**: the MQTT hub lives on the display unit. If it reboots or loses WiFi, HA control is unavailable (LWT marks it offline) until it reconnects. Scheduled feeding itself is unaffected — it runs on-device.
- **Multiple feeders**: give each display unit a different topic prefix (e.g. `tank1`, `tank2`) and HA separates them cleanly.
- **No TLS yet**: plain MQTT on the LAN. If your broker requires TLS, put a plain-Listener on the LAN side (e.g. Mosquitto `listener 1883` on the LAN interface) or wait for a future phase.

## Testing with mosquitto

```sh
# watch everything
mosquitto_sub -h <broker> -t 'catfish/#' -v

# feed manually
mosquitto_pub -h <broker> -t 'catfish/feed/cmd' -m 'ON'

# change default pulse to 5 s
mosquitto_pub -h <broker> -t 'catfish/pulse/set' -m '5'
```
