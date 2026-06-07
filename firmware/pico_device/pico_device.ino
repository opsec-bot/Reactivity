// pico_device.ino — Phase 2 + 3a: USB HID mouse + CDC JSON command channel.
//
// Hardware: Raspberry Pi Pico H (RP2040). Presents to the PC as a USB HID mouse
// (via TinyUSB) AND a USB CDC serial port. It:
//   - replays framed mouse events from the ESP32 (UART0) to the PC  [Phase 2]
//   - speaks a line-based JSON command protocol on the CDC port      [Phase 3]
//     (move / click / scroll / status / watch / remap), driven by the
//     Tauri "Superlight Control" app.
//
// Build:  --fqbn rp2040:rp2040:rpipico:flash=2097152_0,usbstack=tinyusb
//
// Wiring (see docs/notes.md):
//   ESP32 GPIO47 (TX) -> Pico GP1 (UART0 RX, physical pin 2)
//   ESP32 GPIO48 (RX) <- Pico GP0 (UART0 TX, physical pin 1)   [remap, Phase 3b]
//   GND <-> GND
//
// UART frame (from ESP32), per HANDOFF §9:
//   [0xAA][0x55][len=7][type=0x01][buttons, dxLE16, dyLE16, wheel, hwheel][crc8]
//
// CDC protocol (PC <-> Pico), HANDOFF §9 — one JSON object per line:
//   PC->Pico : {"cmd":"move","dx":100,"dy":-50}  {"cmd":"click","btn":"left"}
//              {"cmd":"scroll","wheel":1}  {"cmd":"status"}  {"cmd":"watch","on":true}
//              {"cmd":"remap","from":"side1","to":"ctrl+c"}
//   Pico->PC : {"type":"ack","what":"move"}   {"type":"status",...}
//              {"type":"evt","kind":"mouse","buttons":1,"dx":3,"dy":-2,"wheel":0}

#include <Adafruit_TinyUSB.h>
#include <string.h>
#include <stdlib.h>

// ---- Custom HID report descriptor: 5-button relative mouse, 16-bit X/Y ----
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

typedef struct __attribute__((packed)) {
  uint8_t buttons;
  int16_t x;
  int16_t y;
  int8_t  wheel;
  int8_t  pan;
} mouse_report_t;

Adafruit_USBD_HID usb_hid(desc_hid_report, sizeof(desc_hid_report),
                          HID_ITF_PROTOCOL_MOUSE, 1, false);

// ---- UART frame parser ----
static const uint8_t SOF0 = 0xAA, SOF1 = 0x55;
static const uint8_t TYPE_HID_MOUSE = 0x01;
static const uint8_t TYPE_PC_CMD    = 0x02;  // PC -> ESP (reverse channel)

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

enum PState { P_SOF0, P_SOF1, P_LEN, P_BODY, P_CRC };
static PState pstate = P_SOF0;
static uint8_t pbuf[64];   // [0]=len, [1]=type, [2..]=payload
static uint8_t pidx = 0;
static uint8_t plen = 0;

static unsigned long frames_ok = 0, frames_bad = 0;

// ---- Phase 3 state ----
static bool g_watch = false;             // live event streaming over CDC
static bool g_release_pending = false;   // non-blocking click release
static unsigned long g_release_due = 0;

// Send one HID report. Shared by passthrough + injected commands.
static void sendHid(uint8_t buttons, int16_t x, int16_t y, int8_t wheel, int8_t pan) {
  if (!usb_hid.ready()) return;
  mouse_report_t r{ buttons, x, y, wheel, pan };
  usb_hid.sendReport(0, &r, sizeof(r));
}

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

  // Live stream to the GUI — but NEVER block the hot path. Only write when the
  // CDC TX FIFO has room for the whole line; otherwise drop this event.
  if (g_watch) {
    char e[96];
    int n = snprintf(e, sizeof(e),
      "{\"type\":\"evt\",\"kind\":\"mouse\",\"buttons\":%u,\"dx\":%d,\"dy\":%d,\"wheel\":%d}\n",
      (unsigned)r.buttons, (int)r.x, (int)r.y, (int)r.wheel);
    if (n > 0 && Serial.availableForWrite() >= n) Serial.write((const uint8_t *)e, n);
  }
}

static void feed(uint8_t b) {
  switch (pstate) {
    case P_SOF0: if (b == SOF0) pstate = P_SOF1; break;
    case P_SOF1: pstate = (b == SOF1) ? P_LEN : P_SOF0; break;
    case P_LEN:
      plen = b;
      if (plen > sizeof(pbuf) - 2) { pstate = P_SOF0; break; }
      pbuf[0] = b;
      pidx = 1;
      pstate = P_BODY;
      break;
    case P_BODY:
      pbuf[pidx++] = b;
      if (pidx >= (uint8_t)(2 + plen)) pstate = P_CRC;
      break;
    case P_CRC: {
      uint8_t want = crc8(pbuf, pidx);
      if (want == b) { frames_ok++; handleFrame(pbuf[1], &pbuf[2], plen); }
      else           { frames_bad++; }
      pstate = P_SOF0;
      break;
    }
  }
}

// ---- Minimal JSON readers (flat objects, our own producer = clean input) ----
static const char *jsonValue(const char *json, const char *key) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t') p++;
  if (*p != ':') return nullptr;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}
static bool jsonStr(const char *json, const char *key, char *out, size_t outsz) {
  const char *p = jsonValue(json, key);
  if (!p || *p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
  out[i] = 0;
  return true;
}
static bool jsonInt(const char *json, const char *key, long *out) {
  const char *p = jsonValue(json, key);
  if (!p) return false;
  *out = strtol(p, nullptr, 10);
  return true;
}
static bool jsonBool(const char *json, const char *key, bool *out) {
  const char *p = jsonValue(json, key);
  if (!p) return false;
  *out = (strncmp(p, "true", 4) == 0);
  return true;
}

static uint8_t btnBit(const char *b) {
  if (!strcmp(b, "left"))   return 0x01;
  if (!strcmp(b, "right"))  return 0x02;
  if (!strcmp(b, "middle")) return 0x04;
  if (!strcmp(b, "side1"))  return 0x08;
  if (!strcmp(b, "side2"))  return 0x10;
  return 0;
}

static void ackWhat(const char *what) {
  char b[48];
  int n = snprintf(b, sizeof(b), "{\"type\":\"ack\",\"what\":\"%s\"}\n", what);
  if (n > 0) Serial.write((const uint8_t *)b, n);
}

static void emitStatus() {
  char b[160];
  int n = snprintf(b, sizeof(b),
    "{\"type\":\"status\",\"uptime_ms\":%lu,\"esp_alive\":%s,\"frames_ok\":%lu,\"frames_bad\":%lu,\"watching\":%s}\n",
    millis(), frames_ok > 0 ? "true" : "false", frames_ok, frames_bad,
    g_watch ? "true" : "false");
  if (n > 0) Serial.write((const uint8_t *)b, n);
}

// Forward a PC command to the ESP32 over the reverse UART (Pico GP0 -> ESP IO48),
// §9 type=0x02. Harmless until Phase 3b wires that pin + ESP-side handling.
static void sendEspCmd(const char *json, uint8_t len) {
  if (len > 200) len = 200;
  uint8_t frame[205];
  frame[0] = SOF0; frame[1] = SOF1; frame[2] = len; frame[3] = TYPE_PC_CMD;
  memcpy(&frame[4], json, len);
  frame[4 + len] = crc8(&frame[2], 2 + len);
  Serial1.write(frame, 5 + len);
}

static void injectClick(uint8_t bit) {
  sendHid(bit, 0, 0, 0, 0);          // press
  g_release_due = millis() + 40;     // release later, without blocking
  g_release_pending = true;
}

static void handleCommand(const char *json) {
  char cmd[16];
  if (!jsonStr(json, "cmd", cmd, sizeof(cmd))) return;

  if (!strcmp(cmd, "move")) {
    long dx = 0, dy = 0;
    jsonInt(json, "dx", &dx);
    jsonInt(json, "dy", &dy);
    sendHid(0, (int16_t)dx, (int16_t)dy, 0, 0);
    ackWhat("move");
  } else if (!strcmp(cmd, "click")) {
    char btn[12] = {0};
    jsonStr(json, "btn", btn, sizeof(btn));
    injectClick(btnBit(btn));
    ackWhat("click");
  } else if (!strcmp(cmd, "scroll")) {
    long w = 0;
    jsonInt(json, "wheel", &w);
    sendHid(0, 0, 0, (int8_t)w, 0);
    ackWhat("scroll");
  } else if (!strcmp(cmd, "status")) {
    emitStatus();
  } else if (!strcmp(cmd, "watch")) {
    bool on = false;
    jsonBool(json, "on", &on);
    g_watch = on;
    ackWhat("watch");
  } else if (!strcmp(cmd, "remap")) {
    sendEspCmd(json, (uint8_t)strlen(json));
    ackWhat("remap");
  } else {
    Serial.println("{\"type\":\"error\",\"msg\":\"unknown_cmd\"}");
  }
}

static char cmdbuf[256];
static size_t cmdlen = 0;

static void pollCdc() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      cmdbuf[cmdlen] = 0;
      if (cmdlen) handleCommand(cmdbuf);
      cmdlen = 0;
    } else if (c != '\r') {
      if (cmdlen < sizeof(cmdbuf) - 1) cmdbuf[cmdlen++] = (char)c;
      else cmdlen = 0;  // overflow guard
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Phase 4: present to the PC as a generic "Logitech USB Receiver" (046D:C547)
  // — the same identity the real Superlight dongle would show. Set BEFORE the
  // USB device enumerates. Generic Logitech ID per HANDOFF §11.3 (no
  // cheat-device VID/PIDs). On the arduino-pico core USB is already initialized
  // by the time setup() runs, so detach/attach forces the host to re-read the
  // new descriptors.
  TinyUSBDevice.setManufacturerDescriptor("Logitech");
  TinyUSBDevice.setProductDescriptor("USB Receiver");
  TinyUSBDevice.setID(0x046D, 0xC547);
  TinyUSBDevice.detach();
  delay(20);
  TinyUSBDevice.attach();

  Serial.begin(115200);       // USB CDC: command channel + console
  Serial1.begin(1000000);     // UART0 from the ESP32 (GP1 RX / GP0 TX)

  usb_hid.begin();

  unsigned long t0 = millis();
  while (!TinyUSBDevice.mounted() && millis() - t0 < 3000) delay(10);
}

void loop() {
  // Drain the ESP32 UART each pass to minimize passthrough latency.
  while (Serial1.available()) feed((uint8_t)Serial1.read());

  // Service the PC command channel.
  pollCdc();

  // Non-blocking release for an injected click.
  if (g_release_pending && (long)(millis() - g_release_due) >= 0) {
    sendHid(0, 0, 0, 0, 0);
    g_release_pending = false;
  }

  // Periodic status so the GUI shows live frame counters.
  static unsigned long last = 0;
  if (millis() - last > 1000) {
    last = millis();
    emitStatus();
  }
}
