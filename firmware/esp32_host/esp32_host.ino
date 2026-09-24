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
//
// Latency design (Phase 5). EspUsbHost::task() must NOT be called from loop():
// it blocks 1 tick in usb_host_lib_handle_events() and 1 tick in
// usb_host_client_handle_events() every pass, and only re-submits the IN
// transfer on a millis() timer ((now-last) > interval => every >=2 ms). That
// caps the dongle at a few hundred reports/s and delays each one by up to a
// tick. Instead:
//   - usbClientTask (high priority) blocks in usb_host_client_handle_events()
//     and wakes the instant a transfer completes;
//   - onReceive() copies the report, RE-ARMS THE TRANSFER IMMEDIATELY, and only
//     then remaps + forwards, so the host controller polls the dongle again on
//     the very next 1 ms frame;
//   - MOUSE_URB_DEPTH (3) URBs are kept queued on the endpoint, not one — a
//     single URB capped the feed at ~500/s (see the comment on that constant);
//   - only the mouse endpoint (0x81) is armed — the keyboard/HID++ endpoints
//     carry nothing this bridge uses;
//   - loop() does the slow work (LED, logging, reverse-UART commands) at low
//     priority and can never delay a report.

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

static volatile unsigned long g_frames = 0;

// Mouse IN URBs kept queued on EP 0x81. MEASURED on this hardware (Superlight
// dongle, 1 kHz): one URB at a time = ~500 reports/s even with an immediate
// re-arm in the callback; three queued = ~960-990/s. (Presumably a poll slot goes
// empty while a finished URB is resubmitted; several queued URBs keep one ready
// every frame — the mechanism is inferred, the numbers are measured.) Slot 0 is
// the library's own transfer; the rest are allocated here. Set to 1 to get the
// single-URB behaviour back.
// Touched only from the USB client task (callbacks, arming and onGone all run
// inside usb_host_client_handle_events()), so no locking.
static const int MOUSE_URB_DEPTH = 3;
static usb_transfer_t *g_urb[MOUSE_URB_DEPTH] = {};
static bool g_inflight[MOUSE_URB_DEPTH] = {};

// Endpoint recovery. A failed transfer (ERROR / STALL / OVERFLOW / TIMED_OUT —
// e.g. a glitch while the mouse is switched off or on) HALTS the pipe in
// ESP-IDF, and every later submit fails until the endpoint is cleared. Without
// this the bridge sat on "dongle present, no reports" (steady dim green) until
// the board was power-cycled. Same task-only access rule as above.
static bool g_ep_halted = false;
static int  g_last_bad_status = -1;             // for the log line in loop()
static volatile unsigned long g_recoveries = 0;
static unsigned long g_stuck_since = 0;          // first failed re-arm, 0 = healthy
static const unsigned long STUCK_RESTART_MS = 1500;

// --- Phase 3b: button remap (intercept). g_remap[src] = dst button index, or
//     -1 to drop ("none"). Identity by default = plain passthrough.
//     Button index: 0=left 1=right 2=middle 3=side1 4=side2.
//     (Written by loop(), read by the USB client task -> volatile.)
static volatile int8_t g_remap[5] = { 0, 1, 2, 3, 4 };

// --- Status LED state (written by the USB client task, read by loop()) ---
static volatile unsigned long g_last_frame_ms = 0;  // last report from the dongle
static volatile bool g_dongle_present = false;      // seen a frame, not yet 'gone'

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
  // Make sure every mouse URB slot is queued. Cheap when they all are (a few
  // bool checks), so the client task runs it on every wake-up as a watchdog.
  // Runs only in the USB client task (the same task that mutates usbTransfer[]
  // on connect/disconnect), so there is no race with the library's bookkeeping.
  void armIfNeeded() {
    if (!isReady || usbTransferSize == 0) return;

    // Slot 0 = the library's own mouse transfer (a fresh pointer after every reconnect).
    if (!g_inflight[0]) {
      g_urb[0] = nullptr;
      for (int i = 0; i < usbTransferSize; i++)
        if (usbTransfer[i] && usbTransfer[i]->bEndpointAddress == MOUSE_EP) { g_urb[0] = usbTransfer[i]; break; }
    }
    usb_transfer_t *base = g_urb[0];
    if (!base) return;

    if (g_ep_halted) {
      // halt (no-op if the error already halted it) -> flush (queued URBs come
      // back as CANCELED, clearing their g_inflight) -> clear (pipe usable again).
      usb_host_endpoint_halt(base->device_handle, MOUSE_EP);
      usb_host_endpoint_flush(base->device_handle, MOUSE_EP);
      usb_host_endpoint_clear(base->device_handle, MOUSE_EP);
      g_ep_halted = false;
      g_recoveries = g_recoveries + 1;
    }

    bool failed = false;
    for (int k = 0; k < MOUSE_URB_DEPTH; k++) {
      if (g_inflight[k]) continue;
      if (k > 0) {
        if (!g_urb[k] && usb_host_transfer_alloc(base->data_buffer_size, 0, &g_urb[k]) != ESP_OK) {
          g_urb[k] = nullptr;
          continue;
        }
        // Clone the library's transfer (it is only re-pointed at the current device
        // handle here, never freed, so a late "cancelled" callback can't hit freed memory).
        usb_transfer_t *x = g_urb[k];
        x->device_handle    = base->device_handle;
        x->bEndpointAddress = base->bEndpointAddress;
        x->callback         = base->callback;
        x->context          = base->context;
        x->num_bytes        = base->num_bytes;
        x->timeout_ms       = base->timeout_ms;
        x->flags            = base->flags;
      }
      g_inflight[k] = (usb_host_transfer_submit(g_urb[k]) == ESP_OK);
      if (!g_inflight[k]) failed = true;
    }

    // Last resort: if the endpoint still refuses transfers after a clear (e.g. a
    // real device-side STALL, which needs a CLEAR_FEATURE this sketch doesn't
    // send), reboot. The dongle is re-enumerated from scratch, about 1 s of downtime.
    if (!failed) {
      g_stuck_since = 0;
    } else if (!g_stuck_since) {
      g_stuck_since = millis() | 1;
    } else if (millis() - g_stuck_since > STUCK_RESTART_MS) {
      Serial.println("[esp32] mouse endpoint stuck, restarting");
      Serial.flush();
      esp_restart();
    } else {
      g_ep_halted = true;   // try the halt/flush/clear cycle again on the next pass
    }
  }

  // Which URB slot does this completed transfer belong to (-1 = none)?
  static int urbSlot(const usb_transfer_t *t) {
    for (int k = 0; k < MOUSE_URB_DEPTH; k++) if (g_urb[k] == t) return k;
    return -1;
  }

  // Called by the library from usb_host_client_handle_events(), i.e. inside the
  // USB client task, the moment the dongle's IN transfer completes.
  void onReceive(const usb_transfer_t *transfer) override {
    // Only the mouse interface (EP 0x81). Ignore keyboard (0x82) / HID++ (0x83).
    if (transfer->bEndpointAddress != MOUSE_EP) return;

    const int slot = urbSlot(transfer);
    if (slot >= 0) g_inflight[slot] = false;   // this URB just left the queue

    // Cancelled / stalled / device gone: do NOT resubmit from here (the library
    // may be about to free the transfer). The client task re-queues it via
    // armIfNeeded() once the device is healthy again. A real transfer error
    // halted the pipe, so flag it for the halt/flush/clear cycle there.
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      if (transfer->status != USB_TRANSFER_STATUS_CANCELED &&
          transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        g_ep_halted = true;
        g_last_bad_status = transfer->status;
      }
      return;
    }

    // 1. Copy out what we need...
    uint8_t d[8];
    if (transfer->actual_num_bytes < (int)sizeof(d)) {
      if (slot >= 0) g_inflight[slot] = (usb_host_transfer_submit((usb_transfer_t *)transfer) == ESP_OK);
      return;
    }
    memcpy(d, transfer->data_buffer, sizeof(d));

    // 2. ...and put the URB straight back in the queue, so the host controller
    //    keeps polling the dongle every 1 ms frame while we parse and forward.
    if (slot >= 0) g_inflight[slot] = (usb_host_transfer_submit((usb_transfer_t *)transfer) == ESP_OK);

    // 3. Now the actual work.
    g_last_frame_ms = millis();
    g_dongle_present = true;

    uint8_t  buttons = applyRemap(d[0]);   // intercept: rewrite buttons per remap table
    int16_t  dx = (int16_t)(d[2] | (d[3] << 8));
    int16_t  dy = (int16_t)(d[4] | (d[5] << 8));
    int8_t   wheel  = (int8_t)d[6];
    int8_t   hwheel = (int8_t)d[7];

    sendMouseFrame(buttons, dx, dy, wheel, hwheel);
    g_frames = g_frames + 1;   // single writer (this task); volatile ++ is deprecated
  }

  void onGone(const usb_host_client_event_msg_t *) override {
    Serial.println("[esp32] dongle disconnected");
    g_dongle_present = false;
    g_ep_halted = false;
    g_stuck_since = 0;
    // The library just flushed the endpoint and freed ITS transfer; ours were
    // cancelled with it. Nothing is queued any more; armIfNeeded() waits for a
    // new device. (Our extra URBs are kept allocated and reused on reconnect.)
    for (int k = 0; k < MOUSE_URB_DEPTH; k++) g_inflight[k] = false;
    g_urb[0] = nullptr;
  }
};

MouseForwarder dongle;

// --- USB host service tasks (replace EspUsbHost::task(); see header) ---------
// Client task: blocks until the client has an event (transfer done, device
// added/removed) and wakes IMMEDIATELY. The 2 ms timeout is only a watchdog
// tick so armIfNeeded() can (re)start the transfer after a (re)connect or error.
static void usbClientTask(void *) {
  for (;;) {
    usb_host_client_handle_events(dongle.clientHandle, pdMS_TO_TICKS(2));
    dongle.armIfNeeded();
  }
}

// Library task: enumeration / hub events. Not latency critical; blocks forever
// until there is something to do instead of burning a tick per pass.
static void usbLibTask(void *) {
  uint32_t flags;
  for (;;) usb_host_lib_handle_events(portMAX_DELAY, &flags);
}

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
  // With no TX ring buffer, uart_write_bytes() blocks until the frame has
  // physically left the wire (~120 us). A ring buffer makes write() a memcpy.
  Serial1.setTxBufferSize(256);
  Serial1.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);  // link to Pico

  rgb.begin();
  rgb.clear();
  rgb.show();

  delay(1500);
  Serial.println();
  Serial.println("===== Phase 2: Mouse Forwarder (DevKitC-1 host) =====");
  Serial.println("Waiting for dongle on native USB (GPIO19/20). Move the Superlight.");

  dongle.begin();

  // Service the USB host from dedicated tasks pinned to THIS core (usb_host_install
  // above allocated the USB interrupt here). The client task outranks everything
  // in the sketch, so loop() can never delay a report.
  const BaseType_t core = xPortGetCoreID();
  xTaskCreatePinnedToCore(usbLibTask,    "usb_lib",    4096, nullptr, 10, nullptr, core);
  xTaskCreatePinnedToCore(usbClientTask, "usb_client", 6144, nullptr, 20, nullptr, core);
}

// Everything in loop() is cold path: LED, logging, remap commands from the PC.
// The USB host is serviced by usbClientTask/usbLibTask (see setup()).
void loop() {
  // Reverse channel: remap commands forwarded from the Pico (GP0 -> GPIO48).
  while (Serial1.available()) rfeed((uint8_t)Serial1.read());

  updateLed();

  // Throttled heartbeat. The per-second rate is the number to watch: while the
  // mouse is moving it should sit near 1000/s (the dongle's poll rate).
  static unsigned long last = 0, last_frames = 0;
  unsigned long now = millis();
  if (now - last > 1000) {
    unsigned long f = g_frames;
    Serial.printf("[esp32] mouse frames sent: %lu (%lu/s)\n", f, (f - last_frames) * 1000UL / (now - last));
    static unsigned long seen_recoveries = 0;
    unsigned long r = g_recoveries;
    if (r != seen_recoveries) {
      Serial.printf("[esp32] endpoint recovered from transfer error (status %d), %lu total\n",
                    g_last_bad_status, r);
      seen_recoveries = r;
    }
    last = now;
    last_frames = f;
  }

  vTaskDelay(pdMS_TO_TICKS(1));  // yield; nothing here needs to run faster than 1 kHz
}
