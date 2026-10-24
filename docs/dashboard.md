# Web Dashboard

The display unit serves a built-in, mobile-first dashboard on **http://catfishfeeder.local/** (or `http://<display-ip>/`) — open it from any phone or laptop on the same LAN. No app, no cloud, no port forwarding; the page is embedded in the firmware (gzip, ~4 KB) and served by the device itself.

The dashboard is the LVGL touchscreen's web twin: same feed pipeline, same scheduler, same pairing token. Changes made here appear on the touchscreen within a second — and vice versa.

## Logging in

Commands (feed, schedule changes, firmware updates) require the **pairing token** — the same one the relay unit checks (default `catfish`, change it on the device: Settings → Relay Unit → *Pairing token*). The dashboard asks for it on the first 403 and stores it in your browser's `localStorage`. Status views are open — the appliance is LAN-only by design.

## What you can do

| Card | Actions |
|------|---------|
| Next feeding | Live view: next scheduled time, last feed, "Feeding now..." indicator |
| Feed now | Trigger a feed with a 1-30 s pulse (routes through the same worker as the touchscreen; shows the on-device toast too) |
| Schedule | List up to 8 daily feedings — toggle to pause an entry, delete, add new times; **Save** sends the whole schedule atomically (all-or-nothing validation on the device) |
| System | Firmware version, WiFi/relay/MQTT status lines, and **in-browser firmware updates**: pick a `firmware.bin`, the device flashes it and reboots (same mechanism as ArduinoOTA, but drag-and-drop) |

## HTTP API

Same token header (`X-Catfish-Token`) on every mutating endpoint.

| Endpoint | Auth | Body/Response |
|----------|------|---------------|
| `GET /api/status` | — | Snapshot JSON: wifi (ssid/ip/rssi), relay (online/ip), `nextFeed`, `lastFedEpoch` + `lastFeedIso`, `pulse`, `schedCount`, `feeding`, `clockValid`, `mqtt`/`mqttState`, `version` |
| `GET /api/version` | — | `{"version":"1.3.0","proto":1}` |
| `GET /api/schedule` | — | The live schedule as a JSON array `[{"h":7,"m":30,"e":true,"s":3}, ...]` |
| `PUT /api/schedule` | token | Whole-array replace. `202` accepted (applied by the main loop moments later, all-or-nothing), `400` bad JSON/size, `503` busy |
| `POST /api/feed` | token | `{"seconds":1-30}` optional (default pulse otherwise). `202` queued, `503` busy |
| `POST /api/update` | token | `multipart/form-data` firmware upload → `200 {"ok":true,"rebooting":true}` |

Example:

```sh
curl -X POST http://catfishfeeder.local/api/feed \
     -H 'X-Catfish-Token: your-token' \
     -H 'Content-Type: application/json' -d '{"seconds":5}'

curl -X PUT http://catfishfeeder.local/api/schedule \
     -H 'X-Catfish-Token: your-token' -H 'Content-Type: application/json' \
     -d '[{"h":7,"m":30,"e":true,"s":3},{"h":17,"m":30,"e":true,"s":5}]'
```

## Architecture notes

- The HTTP server runs in its own FreeRTOS task (`web-task`) and **never blocks the UI loop**; it reads a plain-field state cache that `loop()` refreshes every second, so a dashboard request can never stall the touchscreen or the feeding schedule.
- Feed commands and schedule changes flow through the same single-writer path as touch input (FreeRTOS queues → `loop()`), so the LVGL UI, MQTT, and the dashboard can never fight over the scheduler.
- Firmware uploads via the dashboard and ArduinoOTA (port 3232) both use the ESP32 OTA slot — don't run two updates at once.

## Editing the dashboard

The page source lives at [`dashboard/index.html`](../dashboard/index.html); a PlatformIO pre-build hook (`scripts/embed_dashboard.py`) gzip-compresses it into `src/dashboard_html.h` automatically when the source changes — just edit + `pio run`.
