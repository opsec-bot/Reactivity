# Superlight Control (Tauri app)

PC-side control panel for the mouse-passthrough device (Phase 3). Talks to the
**Pico** over its USB-CDC serial port using the §9 JSON-line protocol.

- **Backend:** Rust / Tauri v2. Serial I/O is blocking `serialport` on a
  dedicated `std::thread` — no async/tokio in our code (HANDOFF §11.6). Incoming
  CDC lines are forwarded to the UI as `serial://line` events; connection state
  as `serial://status`.
- **Frontend:** Vite + React 19 + TypeScript + Tailwind v4 + shadcn/ui (dark).

## What it does

- **Connection** — auto-detects the Pico (USB 239A:CAFE) and connects at 115200.
- **Status** — live `uptime / ESP link / frames ok / frames bad` (1 Hz heartbeat).
- **Control** — `move` (dx/dy + nudge pad), `click` (L/R/M/side1/side2), `scroll`.
- **Watch** — live stream of passthrough mouse events.
- **Remap** — sends a `remap` command (takes effect once Phase 3b wires the
  reverse UART + ESP32 handler).
- **Console** — raw rx/tx/sys log.

## Prerequisites

- Rust (stable), Node 20+, pnpm. Tauri v2 system deps (WebView2 ships with
  Windows 10/11).

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
src/                      React UI
  lib/api.ts              typed wrappers over Tauri commands + message types
  hooks/useDevice.ts      connection + event wiring
  components/ui/          shadcn components
src-tauri/src/
  lib.rs                  Tauri commands (move/click/scroll/remap/status/watch)
  serial.rs               port-owning thread (blocking serialport)
```
