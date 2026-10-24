# AI Tooling — MCP servers, Claude Code plugins, devcontainer

> Researched and verified 2025 (Task 4-c). All URLs below were checked live.
> SECURITY: never put API keys or tokens in any committed file — including
> `.mcp.json`, manifests, or workflows. See "Security" in `/CLAUDE.md`.

## `.mcp.json` (repo root, project-scoped Claude Code servers)

Currently registers one server:

```json
{
  "mcpServers": {
    "context7": {
      "type": "http",
      "url": "https://mcp.context7.com/mcp"
    }
  }
}
```

Claude Code asks for approval before using project-scoped `.mcp.json` servers;
approve once per workspace.

### context7 (up-to-date library docs)

- Upstream: <https://github.com/upstash/context7> (default branch: `master`)
- Remote streamable-HTTP MCP endpoint: `https://mcp.context7.com/mcp`
  (verified live; responds 405 to bare GET/HEAD, accepts MCP POST)
- The Claude Code `.mcp.json` schema requires `"type": "http"` for a `url`
  entry — a `url` without a `type` is read as a broken stdio server and
  skipped (per Claude Code MCP docs).
- Optional: a free API key from <https://context7.com/dashboard> raises rate
  limits. It would go in the `Authorization: Bearer <key>` header — do NOT
  commit a key; if needed, use a local-only override (`~/.claude.json`) or an
  env var expansion that is not checked in.
- Tools: `resolve-library-id`, `query-docs`.
- Tip for agents: prefix prompts with "use context7" when asking for library
  docs (e.g. LVGL v8, ArduinoJson v7, ESP-IDF).

### claude-mem (persistent cross-session memory) — PLUGIN, not a repo MCP entry

Decision (researched from <https://github.com/thedotmack/claude-mem>, v15.0.2):
claude-mem is distributed as a **Claude Code plugin** via a plugin marketplace.
Its README does NOT prescribe a standalone `.mcp.json` stdio command; instead
the plugin itself registers its `mcp-search` MCP server (stdio, backed by its
local worker + SQLite/Chroma store) automatically when installed. Its
server script resolves paths inside the installed plugin cache
(`~/.claude/plugins/...`), so duplicating it in the repo `.mcp.json` would be
fragile and redundant.

Install it once per machine, inside Claude Code:

```
/plugin marketplace add thedotmack/claude-mem
/plugin install claude-mem
```

then restart Claude Code. (Alternative machine-wide installer:
`npx claude-mem install`. Note `npm install -g claude-mem` installs the
SDK/library only — it does NOT register the plugin hooks/worker.)

After install you get MCP search tools: `search`, `timeline`,
`get_observations` (3-layer workflow: index → timeline → full details).
Settings live in `~/.claude-mem/settings.json` (user-local, never committed).

## Devcontainer (`.devcontainer/devcontainer.json`)

- Image `mcr.microsoft.com/devcontainers/base:ubuntu-24.04` (tag verified on
  MCR) + feature `ghcr.io/devcontainers/features/python:1` (Python 3.12).
- `postCreateCommand` installs PlatformIO into the user site:
  `python3 -m pip install --user --upgrade platformio && python3 -m platformio --version`
  - Why `python3 -m platformio` and not `pio`: a `pip --user` install puts the
    `pio` script in `~/.local/bin`, which login shells add to PATH but the
    non-login lifecycle shell may not; the module form always works.
  - Why `--user` is safe here: the devcontainers Python feature builds its own
    CPython (no distro `EXTERNALLY-MANAGED` PEP 668 marker). On a *distro*
    python (plain Ubuntu 24.04), `pip install --user` fails with
    "externally-managed-environment" — verified — so do not run this command
    against `/usr/bin/python3` outside the devcontainer.
- Recommended VS Code extensions: `platformio.platformio-ide`,
  `ms-vscode.cpptools`, `figma.figma-vscode-extension` (mirrored in
  `.vscode/extensions.json`).
- ESP-IDF extension deliberately omitted (heavyweight; the Arduino/pioarduino
  toolchain is all this repo needs).

## CI / Pages / web flasher

- `.github/workflows/firmware.yml` ("Firmware") builds both units, merges the
  ESP32-S3 image, deploys `web-flasher/` to GitHub Pages on `main`, and
  attaches bins to `v*` tag releases.
- Pages URL: <https://mlger.github.io/smartfishfeeder/> (Pages build_type:
  `workflow` — official Pages actions only).
- esptool v5 merge syntax (verified against esptool 5.6.0):

  ```
  esptool --chip esp32s3 merge-bin --output merged-firmware.bin \
    --flash-mode dio --flash-size 16MB \
    0x0 bootloader.bin 0x8000 partitions.bin \
    0xe000 boot_app0.bin 0x10000 firmware.bin
  ```

  v5 renamed `merge_bin` → `merge-bin` and `--flash_mode` → `--flash-mode`
  (underscore forms still work but warn). `esptool.py` → `esptool`.
- ESP Web Tools v10: loaded from
  `https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module`
  (verified 200, resolves to 10.4.0). Manifest `chipFamily` must be exactly
  `ESP32-S3` / `ESP8266`; `new_install_prompt_erase: true` shows the
  "Erase device" checkbox in the install dialog. ESP32 (IDF v4+/Arduino 3.x)
  firmware must be pre-merged — ESP Web Tools cannot patch flash parameters
  on the fly.
