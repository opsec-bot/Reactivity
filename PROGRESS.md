# PROGRESS

Running log of what's done and what's known. Newest entries on top.

## Phase 1 — Dongle decode (in progress)

### Done
- Project scaffold created; git initialized.
- Toolchain verified on this machine (`arduino-cli` 1.5.1, esp32 core 3.0.0,
  EspUsbHost 1.0.1, pyserial 3.5, Rust/cargo present).
- COM-port map identified (see `docs/notes.md`).
- Phase 1 dumper firmware in place at `firmware/esp32_host/esp32_host.ino`.
- `scripts/capture_serial.py` ready.

### Open
- [ ] Compile + upload Phase 1 firmware to ESP32 (COM12).
- [ ] Confirm dongle enumerates on USB_HOST; capture descriptors.
- [ ] Capture ≥30 mouse reports covering all 5 buttons + axes + wheel.
- [ ] Write `scripts/decode_reports.py` and verify decoded output.
- [ ] Record this dongle's VID/PID + report byte layout below.

### Findings (fill in as discovered)
- **VID:PID:** _TBD_
- **Report length:** _TBD_
- **Byte layout:** _TBD (buttons / dx / dy / wheel / hwheel offsets)_
- **Quirks:** _TBD_
