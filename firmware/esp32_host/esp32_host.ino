// esp32_host.ino — Phase 2: parse the Lightspeed mouse, forward over UART to Pico.
//
// Hardware: ESP32-S3-DevKitC-1 (N32R16V, WROOM-2) as USB HOST. Enumerates the
// Logitech Lightspeed dongle (046D:C547) on the *native* USB OTG controller,
// parses its 13-byte mouse report (endpoint 0x81), and sends a compact framed
// event over UART1 to the Pico, which replays it to the PC as a USB HID mouse.
//
// (The Phase 1 raw-descriptor dumper version of this file is preserved in git
//  tag `phase-1`. The earlier ESP32-S3-USB-OTG variant is in git history — we
//  switched host boards because the OTG board's free pins need soldering.)
//
// BUILD WITH (generic ESP32S3 Dev Module — there's no DevKitC variant). The
// N32R16V is a WROOM-2 with Octal (OPI) flash, so FlashMode=opi is mandatory or
// it boot-loops. Full rationale in docs/esp32-s3-n32r16v.md.
//   --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashMode=opi,FlashSize=16M,PSRAM=disabled"
//   - USBMode=default  -> USB-OTG controller free for the EspUsbHost host driver
//   - CDCOnBoot=default -> Serial = UART0 = CP210x = the "UART" port = COMx
//                          (survives the OTG PHY being claimed for host)
//
// Power for the dongle: the native USB OTG controller does NOT drive VBUS on
// this board. We feed the dongle 5V from a header pin (5V <- J1 "5V"). See wiring.
//
// Wiring — USB-A female breakout to the DevKitC headers:
//   USB-A  5V  (red)   -> 5V    (J1, pin 21)   [powers the dongle]
//   USB-A  D-  (white) -> GPIO19 (J3, pin 20)  [native USB D-]
//   USB-A  D+  (green) -> GPIO20 (J3, pin 19)  [native USB D+]
//   USB-A  GND (black) -> GND   (J3, pin 21)
//   Keep D+/D- short and equal length.
//
// UART link to the Pico (3.3V, no level shift):
//   ESP32 GPIO47 (TX) -> Pico GP1 (UART0 RX)
//   ESP32 GPIO48 (RX) <- Pico GP0 (UART0 TX)   [reverse channel, Phase 3]
//   GND <-> GND
//
// Mouse report layout (IF0, EP 0x81, 13 bytes, no report ID):
//   [0]=buttons(bit0=L,1=R,2=M,3=back,4=fwd) [1]=btns9-16 [2..3]=dX i16 LE
//   [4..5]=dY i16 LE [6]=wheel i8 [7]=hwheel i8 [8..12]=vendor

#include <EspUsbHost.h>
#include <Adafruit_NeoPixel.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// --- Onboard WS2812 RGB status LED (DevKitC-1 v1.1 = GPIO38; v1.0 used GPIO48,
//     which we need for UART RX — so this assumes v1.1). ---
static const int PIN_RGB = 38;
static Adafruit_NeoPixel rgb(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

// --- UART1 to the Pico ---
static const int PIN_UART_TX = 47;
static const int PIN_UART_RX = 48;
static const uint32_t UART_BAUD = 1000000;  // 1 Mbaud

// --- §9 framing ---
static const uint8_t SOF0 = 0xAA, SOF1 = 0x55;
static const uint8_t TYPE_HID_MOUSE = 0x01;
static const uint8_t TYPE_PC_CMD    = 0x02;  // PC->ESP (remap), via Pico reverse UART
static const uint8_t MOUSE_EP = 0x81;

static unsigned long g_frames = 0;

// --- Phase 3b: button remap (intercept). g_remap[src] = dst button index, or
//     -1 to drop ("none"). Identity by default = plain passthrough.
//     Button index: 0=left 1=right 2=middle 3=side1 4=side2.
static int8_t g_remap[5] = { 0, 1, 2, 3, 4 };

// --- Status LED state ---
static unsigned long g_last_frame_ms = 0;  // last report from the dongle
static bool g_dongle_present = false;      // seen a frame, not yet 'gone'

static bool remapActive() {
  for (int i = 0; i < 5; i++) if (g_remap[i] != i) return true;
  return false;
}

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

// Rewrite the buttons bitfield through the remap table (the intercept).
static uint8_t applyRemap(uint8_t b) {
  uint8_t out = 0;
  for (int i = 0; i < 5; i++) {
    if (b & (1 << i)) {
      int8_t t = g_remap[i];
      if (t >= 0) out |= (uint8_t)(1 << t);  // t < 0 ("none") -> dropped
    }
  }
  return out;
}

static int8_t btnIndex(const char *n) {
  if (!strcmp(n, "left"))   return 0;
  if (!strcmp(n, "right"))  return 1;
  if (!strcmp(n, "middle")) return 2;
  if (!strcmp(n, "side1"))  return 3;
  if (!strcmp(n, "side2"))  return 4;
  if (!strcmp(n, "none"))   return -1;
  return -2;  // unknown (e.g. a keystroke action — not supported yet)
}

// --- Minimal JSON string reader (flat object, our own producer = clean input) ---
static bool jsonStr(const char *json, const char *key, char *out, size_t outsz) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t') p++;
  if (*p != ':') return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
  out[i] = 0;
  return true;
}

// Handle a PC command forwarded from the Pico (currently just "remap").
static void handleEspCmd(const char *json) {
  char from[16] = {0}, to[16] = {0};
  if (!jsonStr(json, "from", from, sizeof from)) return;
  jsonStr(json, "to", to, sizeof to);
  int8_t fi = btnIndex(from);
  int8_t ti = btnIndex(to);
  if (fi < 0) return;                       // source must be a real button
  g_remap[fi] = (ti == -2) ? fi : ti;       // unknown action -> identity (no-op)
  Serial.printf("[esp32] remap %s -> %s  (table: %d %d %d %d %d)\n",
                from, to, g_remap[0], g_remap[1], g_remap[2], g_remap[3], g_remap[4]);
}

// Reverse-UART frame parser (Pico GP0 -> ESP GPIO48). Mirrors the §9 framing;
// only acts on TYPE_PC_CMD.
static void rfeed(uint8_t b) {
  static enum { R_SOF0, R_SOF1, R_LEN, R_BODY, R_CRC } st = R_SOF0;
  static uint8_t buf[220];
  static uint16_t idx = 0;
  static uint8_t len = 0;
  switch (st) {
    case R_SOF0: if (b == SOF0) st = R_SOF1; break;
    case R_SOF1: st = (b == SOF1) ? R_LEN : R_SOF0; break;
    case R_LEN:
      len = b;
      if (len > sizeof(buf) - 2) { st = R_SOF0; break; }
      buf[0] = b; idx = 1; st = R_BODY;
      break;
    case R_BODY:
      buf[idx++] = b;
      if (idx >= (uint16_t)(2 + len)) st = R_CRC;
      break;
    case R_CRC:
      if (crc8(buf, idx) == b && buf[1] == TYPE_PC_CMD) {
        buf[2 + len] = 0;                    // null-terminate the JSON payload
        handleEspCmd((const char *)&buf[2]);
      }
      st = R_SOF0;
      break;
  }
}

// Build and send one HID_MOUSE frame on UART1. Total 12 bytes on the wire
// (~120us @ 1Mbaud), negligible against the 1ms poll budget.
static void sendMouseFrame(uint8_t buttons, int16_t dx, int16_t dy,
                           int8_t wheel, int8_t hwheel) {
  uint8_t frame[12];
  frame[0] = SOF0;
  frame[1] = SOF1;
  frame[2] = 7;                 // payload length
  frame[3] = TYPE_HID_MOUSE;    // type
  frame[4] = buttons;
  frame[5] = (uint8_t)(dx & 0xFF);
  frame[6] = (uint8_t)((dx >> 8) & 0xFF);
  frame[7] = (uint8_t)(dy & 0xFF);
  frame[8] = (uint8_t)((dy >> 8) & 0xFF);
  frame[9] = (uint8_t)wheel;
  frame[10] = (uint8_t)hwheel;
  frame[11] = crc8(&frame[2], 9);  // CRC over [len,type,payload]
  Serial1.write(frame, sizeof(frame));
}

class MouseForwarder : public EspUsbHost {
public:
  void onReceive(const usb_transfer_t *transfer) override {
    // Only the mouse interface (EP 0x81). Ignore keyboard (0x82) / HID++ (0x83).
    if (transfer->bEndpointAddress != MOUSE_EP) return;
    if (transfer->actual_num_bytes < 8) return;

    g_last_frame_ms = millis();
    g_dongle_present = true;

    const uint8_t *d = transfer->data_buffer;
    uint8_t  buttons = applyRemap(d[0]);   // intercept: rewrite buttons per remap table
    int16_t  dx = (int16_t)(d[2] | (d[3] << 8));
    int16_t  dy = (int16_t)(d[4] | (d[5] << 8));
    int8_t   wheel  = (int8_t)d[6];
    int8_t   hwheel = (int8_t)d[7];

    sendMouseFrame(buttons, dx, dy, wheel, hwheel);
    g_frames++;
  }

  void onGone(const usb_host_client_event_msg_t *) override {
    Serial.println("[esp32] dongle disconnected");
    g_dongle_present = false;
  }
};

MouseForwarder dongle;

// Render the status LED. Called at ~50 Hz from loop() — never the hot path, so
// it can't perturb USB-host timing / latency.
//   blue breathing  = no dongle yet / disconnected
//   green           = live (dim breathe idle, bright pulse on movement)
//   purple          = same, but a remap is engaged (intercept active)
static void updateLed() {
  static unsigned long lastDraw = 0;
  unsigned long now = millis();
  if (now - lastDraw < 20) return;
  lastDraw = now;

  // 0..1 slow breathing curve (~1.8s period).
  float breathe = sinf((float)now / 900.0f * 3.14159265f) * 0.5f + 0.5f;

  uint16_t hue;
  uint8_t  val;
  if (!g_dongle_present) {
    hue = 43690;                                  // blue
    val = (uint8_t)(18 + breathe * 60.0f);        // dim breathe
  } else {
    hue = remapActive() ? 50000 : 21845;          // purple : green
    unsigned long since = now - g_last_frame_ms;
    if (since < 160) {                            // recent movement -> activity pulse
      float a = 1.0f - (float)since / 160.0f;
      val = (uint8_t)(55 + a * 200.0f);
    } else {
      val = (uint8_t)(14 + breathe * 34.0f);      // connected + idle -> gentle breathe
    }
  }
  rgb.setPixelColor(0, rgb.gamma32(Adafruit_NeoPixel::ColorHSV(hue, 255, val)));
  rgb.show();
}

void setup() {
  Serial.begin(115200);                                   // debug console -> CP2102 "UART" port -> COMx
  Serial1.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);  // link to Pico

  rgb.begin();
  rgb.clear();
  rgb.show();

  delay(1500);
  Serial.println();
  Serial.println("===== Phase 2: Mouse Forwarder (DevKitC-1 host) =====");
  Serial.println("Waiting for dongle on native USB (GPIO19/20). Move the Superlight.");

  dongle.begin();
}

void loop() {
  dongle.task();

  // Reverse channel: remap commands forwarded from the Pico (GP0 -> GPIO48).
  while (Serial1.available()) rfeed((uint8_t)Serial1.read());

  updateLed();

  // Throttled heartbeat so we can see frames flowing without flooding the hot path.
  static unsigned long last = 0;
  if (millis() - last > 1000) {
    last = millis();
    Serial.printf("[esp32] mouse frames sent: %lu\n", g_frames);
  }
}
