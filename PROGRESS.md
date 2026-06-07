# PROGRESS

Running log of what's done and what's known. Newest entries on top.

## Phase 2 — End-to-end pass-through ✅ WORKING (2026-06-07)

**Moving the Superlight moves the PC cursor through the full chain**, confirmed
on hardware:

```
Superlight ~RF~ Lightspeed dongle --USB host--> DevKitC-1 --UART 1Mbaud--> Pico H --USB HID--> PC
```

### Host board pivot: OTG board → DevKitC-1
- Phase 1 reverse-engineering was done on the **ESP32-S3-USB-OTG** board (dongle
  plugs straight into its USB_HOST port). That board is now **retired**.
- Phase 2 needs a UART wire from the host to the Pico (GPIO47 → GP1). The OTG
  board's free pins (GPIO47/48) are **bare solder pads** — no way to attach a
  jumper without a soldering iron (not on hand). So the host moved to the
  **ESP32-S3-DevKitC-1-N32R16V**, which has pre-soldered headers.
- The dongle now reaches the DevKitC's **native USB host** (GPIO19/20) via a
  **USB-A female breakout** wired to the headers. See `docs/esp32-s3-n32r16v.md`.

### What's verified
- DevKitC boots cleanly with `FlashMode=opi` (WROOM-2 Octal flash) — boot log
  shows `Octal Flash Mode Enabled`. Full FQBN in `docs/esp32-s3-n32r16v.md`.
- **Stage A:** dongle enumerates on native USB host; `frames sent` climbs on
  motion (e.g. 0 → 554 in ~2 s). USB-over-jumpers is electrically fine, no drops.
- **Stage B:** UART link GPIO47→GP1 (+ shared GND). Pico reports
  **`frames ok=N bad=0`** — zero CRC errors across thousands of frames = clean
  link, good ground. Pico HID `usb=ready` (mounted on PC).
- **Cursor moves** when the Superlight moves. ✅

### Gotcha worth remembering
- Opening **COM7** (the DevKitC `UART` port) asserts DTR/RTS and **auto-resets
  the ESP32**, briefly halting the stream. Watch the **Pico (COM14)** during live
  runs — it doesn't reset the chain. (Same DTR auto-reset that makes flashing work.)

### Remaining for Phase 2 sign-off
- Confirm all **5 buttons + scroll** pass through (movement confirmed).
- **Measure added latency** (target ≤2 ms, ≤5 ms hard cap).

---

## Phase 1 — Dongle decode ✅ COMPLETE

### Done
- Project scaffold created; git initialized.
- Toolchain verified (`arduino-cli` 1.5.1, esp32 core 3.0.0, EspUsbHost 1.0.1,
  pyserial 3.5, Rust/cargo present).
- COM-port map identified (see `docs/notes.md`).
- Dumper firmware compiles + uploads from CLI (no IDE GUI).
- Dongle enumerates on USB_HOST; descriptors captured to
  `captures/dongle-descriptors.log`.
- 1592 mouse reports captured to `captures/dongle-reports.log` covering all 5
  buttons + X/Y + wheel.
- `scripts/decode_reports.py` parses the capture and prints decoded events;
  verified L/R/M/back/fwd + wheel±1 + signed dx/dy all correct.

### Findings — THIS dongle (Logitech Lightspeed receiver)
- **VID:PID = `046D:C547`** ("Logitech USB Receiver"). bcdDevice 0x0204, bus
  powered, bMaxPower 98 mA. (Note: C547 is one of the IDs the brief greenlit for
  Phase 4 cloning — handy.)
- **3 HID interfaces, all bInterval=1 (1000 Hz):**
  | IF | EP | Role | Report-desc len |
  |---|---|---|---|
  | 0 | 0x81 | Mouse | 83 B |
  | 1 | 0x82 | Keyboard + Consumer + System | 133 B |
  | 2 | 0x83 | Logitech HID++ vendor (FF00) | 54 B |
- **Mouse report = 13 bytes, NO report ID** (endpoint 0x81):

  | byte | field | type |
  |---|---|---|
  | 0 | buttons 1–8 (bit0=L, bit1=R, bit2=M, bit3=back, bit4=fwd) | u8 bitfield |
  | 1 | buttons 9–16 (unused on Superlight, always 0) | u8 |
  | 2–3 | dX | **int16 LE** |
  | 4–5 | dY | **int16 LE** |
  | 6 | wheel | int8 |
  | 7 | hwheel / AC Pan | int8 |
  | 8–12 | vendor padding | 5×u8 |

- **Quirks / gotchas:**
  - The keyboard interface (EP 0x82) streams idle reports `01 00 00 …` (17 B,
    report ID 1) even when nothing is pressed — these are NOT mouse data. Filter
    by endpoint (mouse = 0x81).
  - **Build flag:** use `USBMode=default` on the `esp32s3usbotg` FQBN, NOT
    `hwcdc`. `hwcdc` sets cdc_on_boot=1 → `Serial` lands on the native USB CDC,
    which we reroute to the host port (invisible). `default` → `Serial` = UART0 →
    CP210x → COM12. (HANDOFF §5 advice was for the generic Dev Module.)
  - **DebugLevel=info** is required for EspUsbHost to print descriptors
    (`_printPcapText` is `#if ARDUHAL_LOG_LEVEL >= INFO`). Per-report flooding is
    gated at VERBOSE, so `info` is the right level: descriptors yes, flood no.
  - 16-bit X/Y means high-speed flicks won't clip at ±127 — maps cleanly onto the
    §9 UART payload `{buttons, dx:i16, dy:i16, wheel:i8, hwheel:i8}`.

### Next (Phase 2)
- ESP32: parse EP 0x81 reports natively in `onReceive`, frame per §9, send over
  UART (GPIO45/46 plan) at 1 Mbaud.
- Pico: TinyUSB HID mouse; receive UART frames, replay to PC.
- Measure latency (target ≤2 ms added, ≤5 ms hard cap).
