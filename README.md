# mouse-passthrough

Hardware mouse pass-through: a Logitech G Pro X Superlight (Lightspeed dongle) is
parsed by an **ESP32-S3-USB-OTG** (USB host), forwarded over UART to a **Raspberry
Pi Pico H** (USB device), which presents to the PC as a wired USB HID mouse + a CDC
serial port. A **Rust CLI** on the PC talks to the Pico over CDC for state queries,
programmatic movement/clicks, and per-button remaps.

This is an input-remapping / accessibility / automation device — **not** a game cheat.

```
[Superlight] ~wireless~ [Lightspeed dongle]
        -> USB_HOST -> ESP32-S3-OTG (host) -> UART -> Pico H (device) -> USB -> PC
                                                                      -> CDC  -> Rust CLI
```

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Blink + serial heartbeat on both boards | ✅ |
| 1 | Dump & decode the dongle's HID reports | 🔄 in progress |
| 2 | ESP32 -> UART -> Pico -> PC HID mouse pass-through | ⏳ |
| 3 | Pico CDC + Rust CLI command channel | ⏳ |
| 4 | Intercept/remap + custom VID/PID + polish | ⏳ |

See `PROGRESS.md` for the running log and `docs/notes.md` for the lab notebook.

## Layout

```
firmware/esp32_host/   ESP32-S3 host-side firmware (Arduino)
firmware/pico_device/  Pico device-side firmware (Phase 2)
rust-client/           PC-side Rust CLI (Phase 3)
scripts/               serial capture + report decoding helpers
captures/              raw serial logs (gitignored)
docs/                  notes & lab notebook
```

## Toolchain

Built with `arduino-cli` (no IDE GUI). Boards/cores: `esp32:esp32`,
`rp2040:rp2040`. See `docs/notes.md` for the exact compile/upload commands and the
COM-port map for this machine.

## License

MIT.
