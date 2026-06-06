// pico_device.ino — Phase 2: USB HID mouse, fed by UART from the ESP32 host.
//
// Hardware: Raspberry Pi Pico H (RP2040). Presents to the PC as a USB HID mouse
// (via TinyUSB). Receives framed mouse events from the ESP32 over UART0 and
// replays them as HID reports — the pass-through.
//
// Build:  --fqbn rp2040:rp2040:rpipico:flash=2097152_0,usbstack=tinyusb
//   usbstack=tinyusb gives us TinyUSB (HID here, + USB CDC `Serial` for free,
//   which Phase 3's Rust client will use).
//
// Wiring (see PROGRESS.md / docs/notes.md):
//   ESP32 GPIO47 (TX) -> Pico GP1 (UART0 RX, physical pin 2)
//   ESP32 GPIO48 (RX) <- Pico GP0 (UART0 TX, physical pin 1)   [Phase 3]
//   GND <-> GND
//
// UART frame (from ESP32), per HANDOFF §9:
//   [0xAA][0x55][len=7][type=0x01][buttons, dxLE16, dyLE16, wheel, hwheel][crc8]
//   crc8 = poly 0x07, init 0x00, computed over [len, type, payload].

#include <Adafruit_TinyUSB.h>

// ---- Custom HID report descriptor: 5-button relative mouse, 16-bit X/Y ----
// 16-bit axes (logical -32767..32767) match the Lightspeed source so fast
// flicks never clip the way an 8-bit (+/-127) mouse would.
static uint8_t const desc_hid_report[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Button)
  0x19, 0x01,        //     Usage Minimum (Button 1)
  0x29, 0x05,        //     Usage Maximum (Button 5)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x95, 0x05,        //     Report Count (5)
  0x75, 0x01,        //     Report Size (1)
  0x81, 0x02,        //     Input (Data,Var,Abs)   -> 5 button bits
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x03,        //     Report Size (3)
  0x81, 0x03,        //     Input (Const,Var,Abs)  -> 3 bit pad
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x16, 0x01, 0x80,  //     Logical Minimum (-32767)
  0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
  0x75, 0x10,        //     Report Size (16)
  0x95, 0x02,        //     Report Count (2)
  0x81, 0x06,        //     Input (Data,Var,Rel)   -> X,Y int16
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x06,        //     Input (Data,Var,Rel)   -> wheel int8
  0x05, 0x0C,        //     Usage Page (Consumer)
  0x0A, 0x38, 0x02,  //     Usage (AC Pan)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x01,        //     Report Count (1)
  0x81, 0x06,        //     Input (Data,Var,Rel)   -> hwheel int8
  0xC0,              //   End Collection
  0xC0               // End Collection
};

// Report payload sent to the host. Packed, no report ID. 7 bytes — note this is
// byte-identical to the UART payload, so we can almost memcpy it across.
typedef struct __attribute__((packed)) {
  uint8_t buttons;
  int16_t x;
  int16_t y;
  int8_t  wheel;
  int8_t  pan;
} mouse_report_t;

// 1ms poll interval => 1000 Hz, matching the source.
Adafruit_USBD_HID usb_hid(desc_hid_report, sizeof(desc_hid_report),
                          HID_ITF_PROTOCOL_MOUSE, 1, false);

// ---- UART frame parser ----
static const uint8_t SOF0 = 0xAA, SOF1 = 0x55;
static const uint8_t TYPE_HID_MOUSE = 0x01;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

// Incremental state machine. We accumulate [len][type][payload...] into buf so
// CRC can be checked over exactly those bytes.
enum PState { P_SOF0, P_SOF1, P_LEN, P_BODY, P_CRC };
static PState pstate = P_SOF0;
static uint8_t pbuf[64];   // [0]=len, [1]=type, [2..]=payload
static uint8_t pidx = 0;
static uint8_t plen = 0;   // payload length

static unsigned long frames_ok = 0, frames_bad = 0;

static void handleFrame(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (type != TYPE_HID_MOUSE || len != 7) return;
  mouse_report_t r;
  r.buttons = payload[0];
  r.x = (int16_t)(payload[1] | (payload[2] << 8));
  r.y = (int16_t)(payload[3] | (payload[4] << 8));
  r.wheel = (int8_t)payload[5];
  r.pan = (int8_t)payload[6];

  if (usb_hid.ready()) {
    usb_hid.sendReport(0, &r, sizeof(r));
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // activity blink
  }
}

static void feed(uint8_t b) {
  switch (pstate) {
    case P_SOF0: if (b == SOF0) pstate = P_SOF1; break;
    case P_SOF1: pstate = (b == SOF1) ? P_LEN : P_SOF0; break;
    case P_LEN:
      plen = b;
      if (plen > sizeof(pbuf) - 2) { pstate = P_SOF0; break; }  // bogus length
      pbuf[0] = b;        // store len for CRC
      pidx = 1;
      pstate = P_BODY;    // next byte is type, then payload
      break;
    case P_BODY:
      pbuf[pidx++] = b;
      // body = type(1) + payload(plen). buffered count = 1(len)+1(type)+plen
      if (pidx >= (uint8_t)(2 + plen)) pstate = P_CRC;
      break;
    case P_CRC: {
      uint8_t want = crc8(pbuf, pidx);  // over [len,type,payload]
      if (want == b) { frames_ok++; handleFrame(pbuf[1], &pbuf[2], plen); }
      else           { frames_bad++; }
      pstate = P_SOF0;
      break;
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // USB CDC debug/console (also the Phase 3 command channel).
  Serial.begin(115200);
  // UART0 from the ESP32. GP0=TX, GP1=RX by default on the Pico.
  Serial1.begin(1000000);

  usb_hid.begin();

  // Give USB a moment to enumerate; don't block forever (PC may not have a
  // terminal open on the CDC port).
  unsigned long t0 = millis();
  while (!TinyUSBDevice.mounted() && millis() - t0 < 3000) delay(10);
}

void loop() {
  // Drain everything available each pass to minimize latency.
  while (Serial1.available()) feed((uint8_t)Serial1.read());

  // Throttled heartbeat on the USB CDC console (not in the hot path).
  static unsigned long last = 0;
  if (millis() - last > 1000) {
    last = millis();
    Serial.printf("[pico] frames ok=%lu bad=%lu  usb=%s\n",
                  frames_ok, frames_bad, usb_hid.ready() ? "ready" : "down");
  }
}
