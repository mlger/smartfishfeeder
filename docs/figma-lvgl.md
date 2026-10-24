# Figma to LVGL — UI Design Pipeline

How the 800x480 touch UI (Feed Now / Schedule / Settings) goes from Figma mockups to LVGL C code running on the Waveshare ESP32-S3-Touch-LCD-4.3B. Build/flash instructions are in [firmware.md](firmware.md); the verified display hardware facts are in [notes/hardware-4.3b.md](notes/hardware-4.3b.md).

## Pipeline overview

```
Figma (800x480 frames)
  -> tag layers as LVGL widgets (LVGL Figma plugin)
  -> export (plugin / LVGL AI)
  -> LVGL project / C code
  -> adapt to LVGL v8 API (firmware is pinned to 8.4.0)
  -> firmware/display-unit/src/ui/
  -> pio run -d firmware/display-unit -t upload
```

## Figma setup

1. Create a Figma account and a design file for the project.
2. Design **at exactly 800x480** — one frame per screen (Feed Now, Schedule, Settings), plus reusable components for buttons, cards, headers. The panel is fixed-resolution RGB565; there is no scaling at runtime.
3. (Optional) Enable Dev Mode for inspection. If you use Claude Code, the Figma Dev Mode MCP server gives AI tools direct design context — setup in [tooling.md](tooling.md).

### Personal access token

Some Figma integrations (REST API, CI exports) use a Figma personal access token:

- Figma: profile menu -> **Settings** -> **Security** -> **Personal access tokens**.
- Create it with the minimum scope needed, copy it once, and store it in a **password manager or a CI secret** — **never commit it to the repo**. `.gitignore` already blocks `.env` and `FIGMA_TOKEN`; treat any file that would carry the token as a secret.
- Rotate it regularly (see [tooling.md](tooling.md)); revoke immediately if it ever lands in a log, screenshot, or commit.

## LVGL Figma plugin

Reference docs: <https://lvgl.io/docs/pro/figma/get-started>

The official LVGL Figma plugin bridges design and code:

1. Install the LVGL plugin from the Figma Community plugins inside your design file.
2. **Tag layers as LVGL widgets** — name/annotate frames and layers so the plugin maps them to widget types (screen, button, label, dropdown, slider, arc, etc.). The get-started guide above shows the exact naming conventions.
3. **Export** the tagged design from the plugin. The output is an LVGL project / C code describing the screens.
4. **Import into the firmware**: copy the generated sources into `firmware/display-unit/src/ui/` and wire the screens into the app entry points.

Expect to hand-edit the generated code — this is a bridge, not a compiler.

## LVGL AI

Reference docs: <https://lvgl.io/docs/pro/ai>

LVGL AI generates LVGL UI code from prompts (screens, layouts, styling) and can work from your Figma context. It is part of the **LVGL Pro offering — check the licensing/subscription terms on that page before using generated output in this repo**, and keep any API keys for it out of the repository (same rules as the Figma token: password manager or CI secret, never committed).

## Previewing without hardware

- **viewer.lvgl.io** — open exported LVGL projects in a browser and click through screens before flashing. Use it to sanity-check layout and navigation between Feed Now / Schedule / Settings.

## Design tokens used by the firmware

Keep mockups and code in sync with these values:

| Token | Value | Usage |
|-------|-------|-------|
| Background | `#10201f` | Screen background (dark teal-black) |
| Accent | `#f2a33c` | Primary buttons, highlights, active states |
| Corner radius | `12` | Cards, buttons, panels |
| Font family | Montserrat | All text |
| Font sizes | 12-32 | Scales with role: labels 12-14, body 16-20, headings 24-32 |
| Screen | 800x480 px, RGB565 | Fixed; design frames at 1:1 |

Exact Montserrat sizes compiled into `include/lv_conf.h`: **10, 12, 14, 16, 20, 24, 26, 30, 32** (sizes 8/18/22/28 are disabled to save RAM). Stick to the enabled sizes or add a size in `lv_conf.h` (`LV_FONT_MONTSERRAT_18 1`, etc.) and rebuild.

## Firmware-side constraints

- The Waveshare port files (`src/lvgl_v8_port.cpp/h`) require the **LVGL v8 API** — `lv_scr_act()`, `lv_btn_create()`, `lv_label_create()`, etc. Plugin/AI output targeting LVGL 9 must be ported back to v8.
- Any code touching LVGL from a non-LVGL task must hold the port lock: `lvgl_port_lock(-1)` ... `lvgl_port_unlock()`.
- `LV_MEM_SIZE` is 64 KB in `include/lv_conf.h`; keep screens lean and prefer creating widgets once at boot over per-frame allocation.
- The backlight is an on/off switch (CH422G expander), not PWM — do not design dimming states.

## Recommended workflow

1. **Mock** all three screens in Figma at 800x480 using the design tokens above.
2. **Export** via the LVGL Figma plugin (tagged layers) into LVGL C code; validate in viewer.lvgl.io.
3. **Adapt** the generated code to the LVGL v8 API used by the firmware and place it in `firmware/display-unit/src/ui/`.
4. **Compile and test** on-device (`pio run -d firmware/display-unit -t upload`); fix spacing/touch targets against the real panel — 800x480 mockups always look smaller in Figma than in your hand.
5. Commit both the Figma file link (in the PR description) and the generated sources, so design and firmware stay reviewable together.
