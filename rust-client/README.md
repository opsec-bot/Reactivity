# Superlight Control (Tauri app)

The PC-side control panel for Reactivity. It talks to the **Pico** over its USB-CDC serial
port using newline-delimited JSON (see [Protocol](../README.md#protocol)).

- **Backend:** Rust / Tauri v2. Serial I/O uses blocking `serialport` on a dedicated
  `std::thread`; there is no async/tokio in our code (HANDOFF §11.6). Incoming CDC lines reach
  the UI as `serial://line` events and connection state as `serial://status`. Firmware runs
  stream in as `flash://event`.
- **Frontend:** Vite + React 19 + TypeScript + Tailwind v4 + shadcn/ui.

## What it does

| Tab | |
|---|---|
| **Control** | `move` (dx/dy + nudge pad), `click` (L/R/M/side1/side2), `scroll` |
| **Watch** | Live stream of passthrough mouse events |
| **Remap** | Sends `remap` rules to the ESP32 through the Pico |
| **Scripts** | Lua editor with examples: run on the board, stop, save to flash (autostart), output pane, API reference |
| **Firmware** | Detect, build, flash and verify both boards (runs `scripts/flash.py --json`) |
| **Console** | Raw rx/tx/sys log |

Connection auto-selects the Pico (`046D:C547`, or `239A:CAFE` on pre-Phase-4 firmware) at
115200 with DTR asserted. The status bar shows uptime, ESP link and frame counters from the
1 Hz heartbeat.

## Prerequisites

- Rust (stable), Node 20+, pnpm. Tauri v2 system deps (WebView2 ships with Windows 10/11).
- For the Firmware tab: Python 3 and `arduino-cli` on `PATH` (override with
  `SUPERLIGHT_PYTHON` / `SUPERLIGHT_FLASH_SCRIPT`).

## Develop

```bash
pnpm install
pnpm tauri dev      # builds Rust + starts Vite, opens the window
```

## Build a release binary

```bash
pnpm tauri build    # produces an .msi / .exe under src-tauri/target/release/bundle
```

## Layout

```
src/
  App.tsx                   shell: connection, status, tabs
  lib/api.ts                typed wrappers over Tauri commands + message types
  lib/scriptExamples.ts     starter Lua scripts + API reference text
  hooks/useDevice.ts        connection + event wiring
  hooks/useFlasher.ts       firmware build/flash runs
  hooks/useScript.ts        script draft, upload progress, board state, output
  components/FirmwarePanel.tsx
  components/ScriptPanel.tsx
  components/ui/            shadcn components
src-tauri/src/
  lib.rs                    Tauri commands (move/click/scroll/remap/status/watch/script_*/flash_*)
  serial.rs                 port-owning thread (blocking serialport)
  flash.rs                  runs scripts/flash.py and forwards its JSON events
```
