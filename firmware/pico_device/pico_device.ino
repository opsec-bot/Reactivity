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
// Latency design (Phase 5):
//   - Reports never get dropped for a busy endpoint. Full-speed USB carries one
//     mouse report per 1 ms frame and the ESP32 frames arrive on their own 1 ms
//     clock, so the two drift and the IN endpoint is often busy at the wrong
//     moment. Everything goes through a small coalescing queue (see OutRpt).
//   - Physical and injected input are merged, not interleaved: buttons are
//     OR'd, motion is summed.
//   - The hot path (UART -> queue -> USB) does no formatting, no CDC writes and
//     no LED I/O; those run after it in loop().
//   - CDC writes are guarded so a PC that stops reading can never stall
//     passthrough (Adafruit_USBD_CDC::write() spins while its FIFO is full).
//   - TinyUSB's tud_task() is already IRQ-driven on this core, so the endpoint
//     becomes ready again without help from loop().
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
static bool g_activity = false;          // a frame arrived (drives the LED, off the hot path)

// ---- Button state: physical (from the ESP32) OR injected (from the PC) ----
// The PC always sees the union, so an injected click never releases a button
// the user is holding, and passthrough never swallows an injected click.
static uint8_t  g_phys_buttons = 0;      // last state reported by the real mouse
static uint8_t  g_inj_buttons  = 0;      // buttons currently held by injected clicks
static uint8_t  g_inj_pending  = 0;      // bit i set = injected button i awaits release
static uint32_t g_inj_release_ms[5];

static inline uint8_t effButtons() { return g_phys_buttons | g_inj_buttons; }

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// ---- Outbound report queue (coalescing) ------------------------------------
//   same button state as the newest entry -> fold the deltas into it
//   button state changed                  -> new entry (press/release edges keep
//                                            their order, so short clicks survive)
//   queue full                            -> fold anyway (never drop motion)
// flushReports() drains the head whenever the IN endpoint is ready. Deltas are
// accumulated wide and sent in int16/int8 chunks, so nothing is ever clipped.
struct OutRpt {
  uint8_t  buttons;
  int32_t  x, y;
  int16_t  wheel, pan;
  uint32_t t_us;        // arrival of the OLDEST report folded in (latency stat)
};
static const uint8_t OQ_SIZE = 16;
static OutRpt oq[OQ_SIZE];
static uint8_t oq_head = 0, oq_count = 0;

// Link / latency stats, published in the 1 Hz status line.
static unsigned long st_merged = 0;      // reports folded into an earlier one
static uint8_t       st_qhwm = 0;        // queue high-water mark since last status
static unsigned long st_sent = 0;        // USB reports sent
static unsigned long st_lat_sum = 0, st_lat_n = 0;
static uint32_t      st_lat_max = 0;     // us from UART frame arrival to USB send

static void enqueueReport(uint8_t buttons, int32_t dx, int32_t dy, int16_t wheel, int16_t pan) {
  // Host not enumerated / asleep: drop instead of accumulating a huge jump that
  // would be dumped on the cursor the moment the PC comes back.
  if (!TinyUSBDevice.mounted()) return;

  if (oq_count) {
    OutRpt &t = oq[(oq_head + oq_count - 1) % OQ_SIZE];
    if (t.buttons == buttons || oq_count == OQ_SIZE) {
      t.buttons = buttons;   // newest state wins (only matters when full)
      t.x += dx;  t.y += dy;
      t.wheel += wheel;  t.pan += pan;
      st_merged++;
      return;
    }
  }
  OutRpt &n = oq[(oq_head + oq_count) % OQ_SIZE];
  n.buttons = buttons;  n.x = dx;  n.y = dy;  n.wheel = wheel;  n.pan = pan;
  n.t_us = micros();
  oq_count++;
  if (oq_count > st_qhwm) st_qhwm = oq_count;
}

// Send the head of the queue if the endpoint is idle. Cheap enough to call on
// every pass and straight after every enqueue.
static void flushReports() {
  if (!oq_count || !usb_hid.ready()) return;
  OutRpt &h = oq[oq_head];
  int32_t cx = clampi(h.x, -32767, 32767);
  int32_t cy = clampi(h.y, -32767, 32767);
  int32_t cw = clampi(h.wheel, -127, 127);
  int32_t cp = clampi(h.pan, -127, 127);
  mouse_report_t r{ h.buttons, (int16_t)cx, (int16_t)cy, (int8_t)cw, (int8_t)cp };
  if (!usb_hid.sendReport(0, &r, sizeof(r))) return;   // lost a race with a busy endpoint: stay queued

  uint32_t lat = micros() - h.t_us;
  st_sent++;  st_lat_sum += lat;  st_lat_n++;
  if (lat > st_lat_max) st_lat_max = lat;

  h.x -= cx;  h.y -= cy;  h.wheel -= (int16_t)cw;  h.pan -= (int16_t)cp;
  if (!h.x && !h.y && !h.wheel && !h.pan) {            // fully drained (a pure button edge drains in one send)
    oq_head = (oq_head + 1) % OQ_SIZE;
    oq_count--;
  }
}

// ---- Watch stream (formatted OFF the hot path) ----
struct Evt { uint8_t buttons; int16_t dx, dy; int8_t wheel; };
static const uint8_t EQ_SIZE = 32;
static Evt eq[EQ_SIZE];
static uint8_t eq_head = 0, eq_count = 0;

static void pushEvt(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel) {
  if (eq_count == EQ_SIZE) return;                     // GUI too slow: drop, never block
  Evt &e = eq[(eq_head + eq_count) % EQ_SIZE];
  e.buttons = buttons;  e.dx = dx;  e.dy = dy;  e.wheel = wheel;
  eq_count++;
}

// CDC helper: write only if the WHOLE line fits. Adafruit_USBD_CDC::write()
// spins while its FIFO is full and a terminal holds DTR, which would freeze
// passthrough if the GUI stops reading.
static void cdcWrite(const char *s, int n) {
  if (n > 0 && Serial.availableForWrite() >= n) Serial.write((const uint8_t *)s, n);
}

static void drainEvts() {
  for (int i = 0; i < 2 && eq_count; i++) {            // small budget per pass
    Evt e = eq[eq_head];
    eq_head = (eq_head + 1) % EQ_SIZE;
    eq_count--;
    char b[96];
    int n = snprintf(b, sizeof(b),
      "{\"type\":\"evt\",\"kind\":\"mouse\",\"buttons\":%u,\"dx\":%d,\"dy\":%d,\"wheel\":%d}\n",
      (unsigned)e.buttons, (int)e.dx, (int)e.dy, (int)e.wheel);
    cdcWrite(b, n);
  }
}

static void handleFrame(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (type != TYPE_HID_MOUSE || len != 7) return;
  int16_t dx    = (int16_t)(payload[1] | (payload[2] << 8));
  int16_t dy    = (int16_t)(payload[3] | (payload[4] << 8));
  int8_t  wheel = (int8_t)payload[5];
  int8_t  pan   = (int8_t)payload[6];

  g_phys_buttons = payload[0];
  enqueueReport(effButtons(), dx, dy, wheel, pan);
  flushReports();          // out on the very next IN poll if the endpoint is idle

  g_activity = true;
  if (g_watch) pushEvt(g_phys_buttons, dx, dy, wheel);
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
  cdcWrite(b, n);
}

// Extra fields (Phase 5) are additive; the app ignores keys it doesn't know.
//   rx_hz / tx_hz : UART frames received / USB reports sent per second, measured
//                   since the previous status line. While the mouse moves,
//                   rx_hz should be ~1000; tx_hz <= rx_hz (coalescing).
//   lat_*_us      : time a report sat in this firmware (UART frame in -> USB
//                   send), avg/max since the previous status line. This is the
//                   Pico's share of the added latency, not the end-to-end total.
//   merged        : total reports folded into an earlier one (was: dropped).
//   q_hwm         : deepest the outbound queue got since the previous status.
static void emitStatus() {
  static unsigned long last_ms = 0, last_ok = 0, last_sent = 0;
  unsigned long now = millis();
  unsigned long dt = now - last_ms;
  if (dt == 0) dt = 1;
  unsigned long rx_hz = (frames_ok - last_ok) * 1000UL / dt;
  unsigned long tx_hz = (st_sent - last_sent) * 1000UL / dt;
  unsigned long lat_avg = st_lat_n ? st_lat_sum / st_lat_n : 0;

  char b[256];
  int n = snprintf(b, sizeof(b),
    "{\"type\":\"status\",\"uptime_ms\":%lu,\"esp_alive\":%s,\"frames_ok\":%lu,\"frames_bad\":%lu,\"watching\":%s,"
    "\"rx_hz\":%lu,\"tx_hz\":%lu,\"lat_avg_us\":%lu,\"lat_max_us\":%lu,\"merged\":%lu,\"q_hwm\":%u}\n",
    now, frames_ok > 0 ? "true" : "false", frames_ok, frames_bad,
    g_watch ? "true" : "false",
    rx_hz, tx_hz, lat_avg, (unsigned long)st_lat_max, st_merged, (unsigned)st_qhwm);
  cdcWrite(b, n);

  last_ms = now;  last_ok = frames_ok;  last_sent = st_sent;
  st_lat_sum = 0;  st_lat_n = 0;  st_lat_max = 0;  st_qhwm = 0;
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

// Press now, release ~40 ms later without blocking. Per-button, so overlapping
// injected clicks each get their own release and never disturb physical buttons.
static void injectClick(uint8_t bit) {
  bit &= 0x1F;
  if (!bit) return;
  int idx = __builtin_ctz(bit);
  g_inj_buttons |= bit;
  g_inj_pending |= bit;
  g_inj_release_ms[idx] = millis() + 40;
  enqueueReport(effButtons(), 0, 0, 0, 0);
  flushReports();
}

static void releaseInjected() {
  if (!g_inj_pending) return;
  uint32_t now = millis();
  bool changed = false;
  for (int i = 0; i < 5; i++) {
    if ((g_inj_pending & (1 << i)) && (int32_t)(now - g_inj_release_ms[i]) >= 0) {
      g_inj_pending &= ~(1 << i);
      g_inj_buttons &= ~(1 << i);
      changed = true;
    }
  }
  if (changed) {
    enqueueReport(effButtons(), 0, 0, 0, 0);
    flushReports();
  }
}

static void handleCommand(const char *json) {
  char cmd[16];
  if (!jsonStr(json, "cmd", cmd, sizeof(cmd))) return;

  if (!strcmp(cmd, "move")) {
    long dx = 0, dy = 0;
    jsonInt(json, "dx", &dx);
    jsonInt(json, "dy", &dy);
    // Merged with whatever the real mouse is doing; big moves are chunked by
    // flushReports() instead of being truncated to int16.
    enqueueReport(effButtons(), dx, dy, 0, 0);
    flushReports();
    ackWhat("move");
  } else if (!strcmp(cmd, "click")) {
    char btn[12] = {0};
    jsonStr(json, "btn", btn, sizeof(btn));
    injectClick(btnBit(btn));
    ackWhat("click");
  } else if (!strcmp(cmd, "scroll")) {
    long w = 0;
    jsonInt(json, "wheel", &w);
    enqueueReport(effButtons(), 0, 0, (int16_t)clampi(w, -32767, 32767), 0);
    flushReports();
    ackWhat("scroll");
  } else if (!strcmp(cmd, "status")) {
    emitStatus();
  } else if (!strcmp(cmd, "watch")) {
    bool on = false;
    jsonBool(json, "on", &on);
    g_watch = on;
    if (!on) eq_count = 0;   // don't replay stale events next time
    ackWhat("watch");
  } else if (!strcmp(cmd, "remap")) {
    sendEspCmd(json, (uint8_t)strlen(json));
    ackWhat("remap");
  } else {
    static const char err[] = "{\"type\":\"error\",\"msg\":\"unknown_cmd\"}\n";
    cdcWrite(err, sizeof(err) - 1);
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
  // The core's default RX FIFO is 32 bytes = under 3 frames at 1 Mbaud. A longer
  // stall (status line, a JSON command) would overrun it and lose frames. Must be
  // set before begin().
  Serial1.setFIFOSize(256);
  Serial1.begin(1000000);     // UART0 from the ESP32 (GP1 RX / GP0 TX)

  usb_hid.begin();

  unsigned long t0 = millis();
  while (!TinyUSBDevice.mounted() && millis() - t0 < 3000) delay(10);
}

void loop() {
  // ---- Hot path: UART -> queue -> USB. Nothing slow may run before this. ----
  while (Serial1.available()) feed((uint8_t)Serial1.read());
  flushReports();          // the endpoint may have become ready since last pass

  // ---- Warm path: PC commands and injected-click releases. ----
  pollCdc();
  releaseInjected();

  // ---- Cold path: formatting, LED, status. Runs after the hot path so it can
  // only ever delay the NEXT pass, and the 256-byte UART FIFO absorbs that. ----
  drainEvts();

  if (g_activity) {        // rate-limited activity blink (was a pin toggle per frame)
    g_activity = false;
    static unsigned long led_ms = 0;
    if (millis() - led_ms >= 40) {
      led_ms = millis();
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }
  }

  // Periodic status so the GUI shows live frame counters.
  static unsigned long last = 0;
  if (millis() - last > 1000) {
    last = millis();
    emitStatus();
  }
}
