# Lab notebook

Free-form notes. Append as we go.

## This machine

- OS: Windows 10 Home (19045)
- `arduino-cli` 1.5.1 at `C:\Users\tav08\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe`
- esp32 core: **3.0.0** installed
- Libraries: EspUsbHost 1.0.1, Adafruit NeoPixel, ESP32 BLE Mouse
- Python 3.13 with pyserial 3.5
- Rust/cargo present

### COM-port map (2026-06-05)

| Port | Device | VID:PID | Role |
|---|---|---|---|
| COM12 | Silicon Labs CP210x USB-UART bridge | 10C4:EA60 | **ESP32 micro-USB port — upload + serial here** |
| COM13 | Espressif native USB CDC | 303A:1001 | ESP32 USB_DEV edge connector (factory firmware). Provides 5V to host port. **Disappears after we flash** (native USB gets rerouted to USB_HOST). |
| COM1 | Motherboard serial | — | ignore |

> Pico H not connected yet (Phase 2). When it appears it'll be an ACM / "USB Serial Device".

## Compile / upload (ESP32 host, Phase 1)

FQBN options per HANDOFF §5: `USBMode=hwcdc CDCOnBoot=default FlashSize=8M PSRAM=disabled`.

```powershell
# Compile
arduino-cli compile `
  --fqbn "esp32:esp32:esp32s3usbotg:USBMode=hwcdc,CDCOnBoot=default,FlashSize=8M,PSRAM=disabled" `
  ".\firmware\esp32_host"

# Upload (serial/UART mode, via the CP210x bridge on COM12)
arduino-cli upload -p COM12 `
  --fqbn "esp32:esp32:esp32s3usbotg:USBMode=hwcdc,CDCOnBoot=default,FlashSize=8M,PSRAM=disabled" `
  ".\firmware\esp32_host"
```

## Capture serial

```powershell
python .\scripts\capture_serial.py COM12 .\captures\dongle-descriptors.log
```

`Serial` on this firmware = UART0 -> CP210x -> COM12 (NOT the native USB port).

## Observations

- (append findings here)
