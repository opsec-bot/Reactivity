# Claude Code Brief: ESP32 + Pico Mouse Pass-through

You are continuing an in-progress hardware project. Read this entire brief first. Then start at §13.

---

## 1. Mission

Build a hardware mouse pass-through device:
- A Logitech G Pro X Superlight (or its Lightspeed dongle) plugs into an **ESP32-S3-USB-OTG** board (USB host side)
- The ESP32 parses HID reports, forwards them over UART to a **Raspberry Pi Pico H** (USB device side)
- The Pico presents to the user's PC as a wired USB HID mouse + a USB CDC serial port
- A **Rust CLI on the PC** talks to the Pico over the CDC serial port for:
  - Querying mouse state
  - Injecting programmatic mouse movements/clicks
  - Configuring per-button remaps (e.g. Mouse4 → Ctrl+C)
- Pass-through latency target: **≤2ms over native** (≈ "1:1 feel")

This is a **legitimate input remapping / accessibility / automation device**, NOT a game cheat. See §11.

---

## 2. User Context

- The user is technically capable: comfortable with Arduino IDE, Python, Rust, Git, and command lines. Treat them as a peer who's new to USB HID internals specifically.
- They prefer **honest engineering trade-offs over salesmanship**. If something might not work, say so.
- They like **terse, direct explanations** with reasoning. They don't need long apologies.
- Tone they've enjoyed in prior conversation: confident, warm, occasionally playful (kiss emojis 💋 are part of the vibe). Match it but don't force it.
- They want to **understand** what's being built, not just receive working code. Comment generously, especially around USB and HID details.
- When they're stuck on a physical-world thing (a cable not seated, an LED not lit), ask them to verify and describe, don't just push past it.

---

## 3. Hardware Inventory (in user's hands)

Already received and known working (Phase 0 blink test passed on both):

| Item | Notes |
|---|---|
| **ESP32-S3-USB-OTG** dev board (official Espressif) | 8MB flash, no PSRAM, ESP32-S3-MINI-1-N8 module. Has LCD, USB_HOST Type-A female port, USB_DEV Type-A male edge connector, micro-USB-to-UART bridge, buttons (UP, DW, MENU, OK, Boot, Reset), GPIO15=green LED, GPIO16=yellow LED |
| **Raspberry Pi Pico H** | RP2040, 2MB flash, 264KB SRAM, pre-soldered headers, comes with micro-USB cable, LED on GP25 |
| **Micro-USB cable** (data-capable, separate purchase) | For ESP32's micro-USB-to-UART bridge port |
| **Jumper wires** | F-F, M-F, M-M assorted (for inter-board UART, etc.) |
| **Logitech G Pro X Superlight** + **Lightspeed dongle** | The mouse to pass through. Dongle is USB-A male. Mouse charges/wired-modes via USB-C. Stock cable is USB-A↔USB-C male/male. |
| **Flipper Zero** | Not part of this build, but the user has it. Ignore unless they bring it up. |

Also already on user's system from a prior side-project (the "mega scanner"):
- Arduino IDE 2.x with ESP32 board package 3.x installed
- Earle Philhower's Raspberry Pi Pico/RP2040 board package installed
- A working Arduino IDE Tools menu the user is familiar with

---

## 4. Project Phases & Current State

| Phase | Description | Status |
|---|---|---|
| 0 | Hardware sanity: blink LED + serial heartbeat on both boards | ✅ Complete |
| 1 | Dump the Lightspeed dongle's HID descriptor + raw report bytes; decode mouse byte layout | 🔄 **CURRENT** |
| 2 | ESP32 forwards parsed mouse events over UART → Pico → PC sees an HID mouse | Not started |
| 3 | Add Pico CDC serial endpoint, write Rust client, plumb command channel through Pico→ESP32 | Not started |
| 4 | Add intercept/remap logic, custom VID/PID for anti-cheat invisibility, polish | Not started |

**The user is stuck at the beginning of Phase 1**: they couldn't find 115200 in Arduino IDE Serial Monitor's baud dropdown, which suggests we should move them off the GUI entirely and use `arduino-cli` from now on (which is what this handoff enables).

---

## 5. CRITICAL Hardware Quirks (ESP32-S3-USB-OTG board)

These trip people up. Internalize them:

1. **No physical DEV/HOST switch.** USB role selection is via GPIO18:
   - GPIO18 LOW (default) = ESP32 USB lines → USB_DEV (Type-A male edge)
   - GPIO18 HIGH = ESP32 USB lines → USB_HOST (Type-A female port)

2. **Micro-USB cable ALONE does not power the USB_HOST port.** Per official Espressif docs: "in this power supply mode, only the motherboard and display are powered."

   To power the USB host port for device enumeration, the user MUST plug something into the **USB_DEV** (Type-A male edge connector) as a 5V power source. Options:
   - Direct into a phone wall wart with a USB-A socket
   - Direct into a USB power bank
   - Into a free USB-A port on their PC (separate from the micro-USB connection)
   - Into a powered USB hub

3. **Required GPIO config in firmware for host mode:**
   ```cpp
   pinMode(13, OUTPUT); digitalWrite(13, LOW);   // BOOST_EN: battery boost off
   pinMode(12, OUTPUT); digitalWrite(12, HIGH);  // DEV_VBUS_EN: pass USB_DEV's 5V to USB_HOST
   pinMode(17, OUTPUT); digitalWrite(17, HIGH);  // LIMIT_EN: enable 500mA current-limit IC
   pinMode(18, OUTPUT); digitalWrite(18, HIGH);  // USB_SEL: route ESP32 USB to USB_HOST connector
   ```
   Without setting these, the dongle will not enumerate even if cabled correctly.

4. **Tools settings for host-mode firmware:**
   - Board: `esp32:esp32:esp32s3usbotg` (arduino-cli FQBN) — or `ESP32S3 Dev Module` in the IDE
   - USB CDC On Boot: **Disabled** (native USB peripheral must be free for host duty)
   - USB Mode: **Hardware CDC and JTAG**
   - Upload Mode: **UART**
   - Flash Size: **8MB**
   - PSRAM: **Disabled** (no PSRAM on this module)
   - Port: the COM port from the USB-to-UART bridge chip (NOT the native USB port)

5. **LEDs available for status:** GPIO15 = green (power-on indicator), GPIO16 = yellow (we use for "traffic" indicator).

---

## 6. Tooling — Use CLI, Not the GUI

The user got stuck on the Arduino IDE GUI. Switch to `arduino-cli` immediately. Install + configure:

```bash
# Install arduino-cli (per platform — adapt the install command)
# Then:
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli core install rp2040:rp2040

# Verify
arduino-cli board listall | grep -i "usb-otg\|pico"
```

Library install:
```bash
arduino-cli lib install "EspUsbHost"@1.0.1
arduino-cli lib install "ArduinoJson"
arduino-cli lib install "Adafruit TinyUSB Library"  # for Pico HID/CDC composite
```

Compile + upload pattern:
```bash
# ESP32 (host side) — adjust FQBN options as needed
arduino-cli compile --fqbn esp32:esp32:esp32s3usbotg:USBMode=hwcdc,CDCOnBoot=default,FlashSize=8M,PSRAM=disabled ./firmware/esp32_host
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32s3usbotg:USBMode=hwcdc ./firmware/esp32_host

# Pico (device side)
arduino-cli compile --fqbn rp2040:rp2040:rpipico:flash=2097152_0,usbstack=tinyusb ./firmware/pico_device
arduino-cli upload -p /dev/ttyACM0 --fqbn rp2040:rp2040:rpipico ./firmware/pico_device
```

Serial capture (instead of GUI Serial Monitor):
```bash
# Capture to file for later parsing
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200 | tee /tmp/esp32-serial.log

# Or use OS tools:
# Linux/Mac: screen /dev/ttyUSB0 115200    (Ctrl-A K to quit)
# Windows: use `mode COMx: BAUD=115200 PARITY=n DATA=8 STOP=1` + a serial capture tool

# Best for Phase 1 dump capture: use Python pyserial to capture cleanly to a file
python3 -c "
import serial, sys
s = serial.Serial(sys.argv[1], 115200, timeout=1)
with open(sys.argv[2], 'w') as f:
    while True:
        line = s.readline().decode(errors='replace')
        if line:
            print(line, end='')
            f.write(line); f.flush()
" /dev/ttyUSB0 /tmp/dongle-dump.log
```

Detect COM ports:
```bash
arduino-cli board list
# or
ls /dev/tty{USB,ACM}* 2>/dev/null   # Linux
ls /dev/cu.usb*                      # macOS
# Windows: arduino-cli board list (shows COMx names)
```

---

## 7. Files Already Written (use as ground truth)

### `firmware/esp32_host/esp32_host.ino` — Phase 1 dongle dumper (CURRENT)

This is the starting firmware for the ESP32. It configures host-mode GPIOs, then dumps every USB descriptor + HID report it sees.

```cpp
// esp32_host.ino — Phase 1: dump descriptors + raw HID reports
#include <EspUsbHost.h>

static const int PIN_USB_SEL     = 18;
static const int PIN_DEV_VBUS_EN = 12;
static const int PIN_BOOST_EN    = 13;
static const int PIN_LIMIT_EN    = 17;
static const int PIN_LED_GREEN   = 15;
static const int PIN_LED_YELLOW  = 16;

class DongleDumper : public EspUsbHost {
public:
  void onReceive(const usb_transfer_t *transfer) override {
    digitalWrite(PIN_LED_YELLOW, !digitalRead(PIN_LED_YELLOW));
    Serial.print("[RAW ");
    Serial.print(transfer->actual_num_bytes);
    Serial.print("] ");
    for (int i = 0; i < transfer->actual_num_bytes; i++) {
      if (transfer->data_buffer[i] < 0x10) Serial.print("0");
      Serial.print(transfer->data_buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  }

  void onGone(const usb_host_client_event_msg_t *) override {
    Serial.println("[DEVICE DISCONNECTED]");
    digitalWrite(PIN_LED_YELLOW, LOW);
  }
};

DongleDumper dongle;

void setup() {
  pinMode(PIN_BOOST_EN,    OUTPUT); digitalWrite(PIN_BOOST_EN,    LOW);
  pinMode(PIN_DEV_VBUS_EN, OUTPUT); digitalWrite(PIN_DEV_VBUS_EN, HIGH);
  pinMode(PIN_LIMIT_EN,    OUTPUT); digitalWrite(PIN_LIMIT_EN,    HIGH);
  pinMode(PIN_USB_SEL,     OUTPUT); digitalWrite(PIN_USB_SEL,     HIGH);
  pinMode(PIN_LED_GREEN,   OUTPUT); digitalWrite(PIN_LED_GREEN,   HIGH);
  pinMode(PIN_LED_YELLOW,  OUTPUT); digitalWrite(PIN_LED_YELLOW,  LOW);
  delay(200);

  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("===== Phase 1: Dongle Descriptor Dumper =====");
  Serial.println("Green LED on = host mode configured.");
  Serial.println("Plug Lightspeed dongle into USB_HOST port.");

  dongle.begin();
}

void loop() {
  dongle.task();
  delay(1);
}
```

Per-board flags reminder (compile/upload commands above): `USBMode=hwcdc CDCOnBoot=default FlashSize=8M PSRAM=disabled`. CDCOnBoot=default in arduino-cli equates to "Disabled" (do not auto-init CDC).

---

## 8. Planned Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                            User's PC                              │
│                                                                   │
│   ┌──────────────┐                            ┌──────────────┐   │
│   │ Rust CLI app │ ◄── COM (CDC) ──────────► │   USB-HID    │   │
│   └──────────────┘                            │ mouse driver │   │
│         │                                     └──────────────┘   │
└─────────┼──────────────────────────────────────────▲────────────┘
          │                                          │
          │     ┌──────────── USB CDC + HID ─────────┘
          │     │
          ▼     ▼
       ┌────────────────┐
       │  Pi Pico H     │
       │  (USB device   │
       │   to PC)       │
       └────────┬───────┘
       UART     │
   (TX/RX/GND)  │
       3 wires  │
                ▼
       ┌────────────────┐         5V power in
       │ ESP32-S3-OTG   │ ◄────── (USB_DEV port)
       │ (USB host      │
       │  for dongle)   │
       └────────┬───────┘
                │ USB_HOST (Type-A female)
                ▼
       ┌────────────────┐
       │ Lightspeed     │ ◄~~~~~ wireless ~~~~► [Superlight mouse]
       │   dongle       │
       └────────────────┘
```

UART pin plan (revisit when wiring):
- ESP32 side: GPIO45 (FREE_1, idle) → Pico GP1 (UART0 RX). ESP32 GPIO46 (FREE_2, idle) ← Pico GP0 (UART0 TX). ESP32 GND ↔ Pico GND.
- Baud: start at 1Mbaud, reduce if errors. Mouse events at 1000Hz × ~6 bytes/event = ~48kbps minimum, plenty of headroom.

---

## 9. Protocol Specifications

### ESP32 ↔ Pico (UART, internal)

Binary frame format, designed for low latency:
```
[0xAA] [0x55] [len:1] [type:1] [payload:len] [crc8:1]
```
- `type=0x01` HID_MOUSE: payload is `{buttons:u8, dx:i16, dy:i16, wheel:i8, hwheel:i8}` (7 bytes)
- `type=0x02` PC_CMD_TO_ESP: from PC via Pico, payload is JSON line
- `type=0x03` ESP_REPLY_TO_PC: response back to Pico for CDC forward

### Pico ↔ PC Rust client (USB CDC, JSON lines)

Same shape as the mega scanner project for consistency. One JSON object per line:
```json
{"cmd": "move", "dx": 100, "dy": -50}
{"cmd": "click", "btn": "left"}
{"cmd": "remap", "from": "side1", "to": "ctrl+c"}
{"cmd": "status"}
```
Replies:
```json
{"type": "ack", "what": "move"}
{"type": "evt", "kind": "mouse", "buttons": 1, "dx": 3, "dy": -2}
{"type": "status", "uptime_ms": 12345, "esp_alive": true, ...}
```

---

## 10. Acceptance Criteria Per Phase

### Phase 1 — Dongle decoded
- [ ] `arduino-cli` workflow operational (ESP32 host firmware compiles + uploads from CLI, no IDE GUI needed)
- [ ] Serial capture script in repo (e.g. `scripts/capture_serial.py`) saves output to `captures/`
- [ ] Lightspeed dongle enumerates when plugged into USB_HOST
- [ ] Descriptors captured to `captures/dongle-descriptors.log`
- [ ] At least 30 raw mouse reports captured to `captures/dongle-reports.log` covering all 5 buttons + axes + wheel
- [ ] A Python or Rust analysis script (`scripts/decode_reports.py`) parses the capture and prints decoded `(buttons, dx, dy, wheel)` for each event
- [ ] Phase 1 firmware committed with phase tag

### Phase 2 — Pass-through working
- [ ] ESP32 firmware parses the dongle's reports natively (not just dumps)
- [ ] Pico firmware presents as USB HID mouse using TinyUSB
- [ ] UART link active between boards, framed protocol from §9
- [ ] User moves Superlight → PC cursor moves through the chain
- [ ] Measured latency: under 5ms from input to PC, ideally ≤2ms (`scripts/latency_test.py` or document the test method)
- [ ] All 5 buttons + scroll work

### Phase 3 — Rust CLI + intercept
- [ ] `rust-client/` Cargo project compiles cleanly (`cargo build --release`)
- [ ] `mouse_ctl move <dx> <dy>` sends command, observable on PC cursor
- [ ] `mouse_ctl click <left|right|middle|side1|side2>` works
- [ ] `mouse_ctl remap <button> <action>` configures intercept rule on ESP32
- [ ] Subscribe / live event stream: `mouse_ctl watch` prints incoming mouse events as JSON

### Phase 4 — Polish
- [ ] Custom VID/PID on Pico: enumerates as a recognized-generic mouse, e.g. `046d:c547` ("Logitech USB Receiver") or a clean unknown vendor pair
- [ ] README with build + flash + usage instructions
- [ ] Tagged release v0.1.0

---

## 11. HARD RULES — Do Not Do

These are explicit user constraints, not negotiable:

1. **Do NOT add any game-cheat-adjacent features.** No memory-reading integrations, no aim-assist hooks, no "DMA" anything, no auto-fire/auto-pull/recoil curves, no aim-snap, no triggerbot. The user has explicitly disclaimed those use cases. The intercept layer is for productivity remaps, accessibility, and automation only.
2. **Do NOT recommend KMBox or similar commercial cheat hardware** as alternatives. The user evaluated and rejected those for anti-cheat-risk reasons.
3. **Do NOT impersonate VID/PIDs of specific anti-cheat-monitored cheat devices.** Generic Logitech / Razer / Microsoft IDs are fine; KMBox / Cronus / Titan-derived ones are not.
4. **Do NOT push the user to solder** if it can be avoided. They have no soldering iron.
5. **Do NOT introduce a breadboard requirement** unless they explicitly buy one. They have jumper wires only.
6. **Do NOT use `tokio` or async runtimes in the Rust client.** Blocking `serialport` is sufficient and matches a prior side-project's style.
7. **Do NOT regress on latency.** Pass-through must stay ≤5ms. If a design choice would push it higher (e.g. heavy JSON parsing in the hot path), call it out before implementing.
8. **Do NOT ask the user to install Logitech G HUB** or any vendor software on the PC. The dongle should work as raw USB HID.

---

## 12. How to Interact with the User

When the user needs to do something physical:
- State it as a numbered checklist
- Wait for confirmation before continuing
- Provide visual cues they can verify (LED states, dongle LED, etc.)

When you discover something they need:
- Tell them, don't quietly buy or install something
- For hardware purchases especially, surface the need + cost + reason

When something fails:
- Capture logs to files in `captures/` or `logs/`
- Read them, propose the diagnosis
- If multiple causes are possible, list them in likelihood order

When in doubt:
- The user is patient with debug iteration but impatient with vagueness. Be specific.

---

## 13. Your First Tasks (in order)

1. **Set up project structure** at `~/projects/mouse-passthrough/` (or wherever the user prefers — ask):
   ```
   mouse-passthrough/
   ├── .gitignore                    (Rust target/, Arduino build/, captures/*.log)
   ├── README.md
   ├── PROGRESS.md                   (you'll update this as we go)
   ├── firmware/
   │   ├── esp32_host/
   │   │   └── esp32_host.ino        (use code in §7)
   │   └── pico_device/              (empty for now, fills in Phase 2)
   ├── rust-client/                  (empty until Phase 3)
   ├── scripts/
   │   ├── capture_serial.py         (write this; use pyserial)
   │   └── decode_reports.py         (write later in Phase 1)
   ├── captures/                     (gitignored except a `.gitkeep`)
   └── docs/
       └── notes.md                  (your free-form lab notebook)
   ```

2. **Initialize git**, make an initial commit.

3. **Install + configure `arduino-cli`** per §6. Verify both cores are installed.

4. **Detect the connected boards** with `arduino-cli board list`. There should be two:
   - The ESP32-S3-USB-OTG showing as the CP210x / CH340 bridge port
   - The Pi Pico showing as ACM / USB serial
   
   If the user has multiple USB serial devices, ask them to unplug everything else briefly to identify which is which. Save the mapping in `docs/notes.md`.

5. **Compile + upload the Phase 1 sketch** to the ESP32. The user must have it cabled correctly per §5:
   - micro-USB → PC
   - USB_DEV → 5V power source (ask them what they have available)
   - Lightspeed dongle → USB_HOST (Type-A female) ← AFTER firmware is running

6. **Start serial capture** to `captures/dongle-descriptors.log` using `scripts/capture_serial.py`.

7. **Ask the user to**: unplug the Superlight dongle from its current PC port (if connected there) and plug it into the OTG board's USB_HOST port. Then move the Superlight in big circles and click each button (L, R, M, side1, side2) at least 3 times each, and scroll the wheel up + down.

8. **Read the capture file**, identify:
   - Device descriptor VID/PID
   - HID Report Descriptor (printed by EspUsbHost in PCAP-text format)
   - Byte layout of mouse reports (which bytes are buttons, dx, dy, wheel)
   
9. **Write `scripts/decode_reports.py`** that parses a `[RAW N] xx xx xx ...` log file and prints decoded events. Verify it produces sensible output for the captured data.

10. **Update `PROGRESS.md`** with what's known about this specific dongle (VID/PID, report layout, any quirks) and commit.

11. **Report back to the user** with the findings before proceeding to Phase 2.

---

## 14. Reference — Decisions Already Made

These are locked, don't re-debate them with the user unless they bring them up:

- **Architecture: two-board (ESP32 host + Pico device).** Single-board BLE was rejected for latency. Single-board USB host+device on ESP32-S3 is hardware-impossible (only one USB PHY).
- **Mouse-only intercept.** Keyboard intercept was deferred. Don't add a USB hub or second host port without being asked.
- **Pass-through target: 1000Hz polling, ≤2ms added latency.** Not pursuing 8000Hz for now.
- **Rust on the PC side.** Not Python, not C#, not AHK.
- **License: MIT.**
- **Hardware ban risk avoidance: custom VID/PID is part of Phase 4, not optional.**

---

## 15. If the User Gets Stuck on a Physical-World Thing

Examples and the correct response:

- "I can't find 115200 in the Serial Monitor dropdown" → switch them to `arduino-cli monitor` or the Python capture script. Don't try to fix the GUI.
- "Nothing prints when I plug in the dongle" → verify in order: (a) is the green LED on? (firmware running), (b) is USB_DEV plugged into a power source? (host port unpowered otherwise), (c) is the dongle's own LED on? (it has one).
- "The Pico isn't showing as a COM port" → it may be in BOOTSEL mode (RPI-RP2 mass storage). After a successful Arduino upload, a real COM port should appear. If not, ask them to press the Reset button on the Pico.
- "Latency feels bad" → measure objectively first. Don't trust feel until we have numbers.

---

End of brief. Begin at §13 task 1.