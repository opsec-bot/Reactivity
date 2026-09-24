# mouse-passthrough

Hardware mouse pass-through: a Logitech G Pro X Superlight (Lightspeed dongle) is
parsed by an **ESP32-S3-DevKitC-1** (USB host), forwarded over UART to a **Raspberry
Pi Pico H** (USB device), which presents to the PC as a wired USB HID mouse + a CDC
serial port. A **Tauri desktop app** (Rust backend + React UI) on the PC talks to the
Pico over CDC for state queries, programmatic movement/clicks, and per-button remaps.

> Phase 1 (dongle reverse-engineering) used an ESP32-S3-USB-OTG board; the host
> moved to the DevKitC-1 for Phase 2 because its headers allow the UART link to the
> Pico without soldering. See `docs/esp32-s3-n32r16v.md`.

This is an input-remapping / accessibility / automation device — **not** a game cheat.

```
[Superlight] ~wireless~ [Lightspeed dongle]
        -> USB host -> ESP32-S3-DevKitC-1 (host) -> UART -> Pico H (device) -> USB -> PC
                                                                            -> CDC  -> Rust CLI
```

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Blink + serial heartbeat on both boards | ✅ |
| 1 | Dump & decode the dongle's HID reports | ✅ (046D:C547, 13B mouse report) |
| 2 | ESP32 -> UART -> Pico -> PC HID mouse pass-through | ✅ cursor moves end-to-end (buttons/scroll + latency to confirm) |
| 3 | Pico CDC + Tauri control app (move/click/scroll/watch) | ✅ 3a done; remap = 3b |
| 4 | Custom VID/PID (046D:C547 "Logitech USB Receiver") + remap intercept | ✅ identity done; release/keystroke-remap polish remain |
| 5 | Low-latency host/device pipeline (1 kHz host polling, no-drop coalescing queue) | ✅ 1 kHz feed verified on hardware (~960/s, was ~280-500); end-to-end latency still unmeasured |

See `PROGRESS.md` for the running log, `docs/notes.md` for the lab notebook, and
`docs/PINOUT.md` for the full wiring/pinout reference with diagram.

## Layout

```
firmware/esp32_host/   ESP32-S3 host-side firmware (Arduino)
firmware/pico_device/  Pico device-side firmware (Phase 2 HID + Phase 3 CDC protocol)
rust-client/           PC-side Tauri app (Rust + React/Tailwind/shadcn) — Phase 3
scripts/               serial capture + report decoding helpers
captures/              raw serial logs (gitignored)
docs/                  notes & lab notebook
```

## Toolchain

Built with `arduino-cli` (no IDE GUI). Boards/cores: `esp32:esp32`,
`rp2040:rp2040`. See `docs/notes.md` for the exact compile/upload commands and the
COM-port map for this machine.

## Flashing

```powershell
python scripts/flash.py              # detect what's plugged in, build it, flash it
python scripts/flash.py --check      # only show what is detected
python scripts/flash.py --only pico  # just one board (esp32 | pico)
python scripts/flash.py --build-only # compile both, touch no hardware
python scripts/flash.py --json       # machine-readable events (for a GUI)
python scripts/test_flash.py         # offline tests for detection/selection
```

The Control app has the same thing in its **Firmware** tab: it detects the boards, shows
build / flash / verify progress with live output, can flash both or just one, closes its
serial connection first and reconnects afterwards. It runs `scripts/flash.py`, so Python
and `arduino-cli` must be available (override with `SUPERLIGHT_PYTHON` /
`SUPERLIGHT_FLASH_SCRIPT`; the script path is otherwise found relative to the source tree).

Plug in the ESP32's **UART** port (the CP210x one) and/or the Pico. A Pico that is
already running this firmware reboots into its bootloader by itself; only a blank Pico
needs BOOTSEL held while plugging in. Close the Control app and any serial monitor
first — an open COM port can't be flashed. Needs `arduino-cli` with the
`esp32:esp32@3.0.0` and `rp2040:rp2040@5.6.0` cores. Failures print a targeted hint
(busy port, ESP32 not in download mode, Pico not in BOOTSEL, missing core).

## License

MIT.
