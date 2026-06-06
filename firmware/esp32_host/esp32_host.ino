// esp32_host.ino — Phase 2: parse the Lightspeed mouse, forward over UART to Pico.
//
// Hardware: ESP32-S3-USB-OTG (USB HOST). Enumerates the Logitech Lightspeed
// dongle (046D:C547) on the USB_HOST port, parses its 13-byte mouse report
// (endpoint 0x81), and sends a compact framed event over UART1 to the Pico,
// which replays it to the PC as a USB HID mouse.
//
// (The Phase 1 raw-descriptor dumper version of this file is preserved in git
//  tag `phase-1`.)
//
// BUILD WITH:  --fqbn esp32:esp32:esp32s3usbotg:USBMode=default
//   USBMode=default => cdc_on_boot=0 => `Serial` = UART0 => CP210x => COMx (debug
//   console). Do NOT use hwcdc (it maps Serial onto the native USB we reroute to
//   the host port). See docs/notes.md.
//
// Power: USB_DEV edge connector must be plugged into a 5V source to power the
// host port. micro-USB provides the debug console (and power).
//
// UART link (3.3V, no level shift):
//   ESP32 GPIO47 (TX) -> Pico GP1 (UART0 RX)
//   ESP32 GPIO48 (RX) <- Pico GP0 (UART0 TX)   [reverse channel, Phase 3]
//   GND <-> GND
//   Chose 47/48 because the board's other free pins (45,46,3) are strapping
//   pins and 26 is a flash pin.
//
// Mouse report layout (IF0, EP 0x81, 13 bytes, no report ID):
//   [0]=buttons(bit0=L,1=R,2=M,3=back,4=fwd) [1]=btns9-16 [2..3]=dX i16 LE
//   [4..5]=dY i16 LE [6]=wheel i8 [7]=hwheel i8 [8..12]=vendor

#include <EspUsbHost.h>

// --- ESP32-S3-USB-OTG board control pins ---
static const int PIN_USB_SEL     = 18;
static const int PIN_DEV_VBUS_EN = 12;
static const int PIN_BOOST_EN    = 13;
static const int PIN_LIMIT_EN    = 17;
static const int PIN_LED_GREEN   = 15;
static const int PIN_LED_YELLOW  = 16;

// --- UART1 to the Pico ---
static const int PIN_UART_TX = 47;
static const int PIN_UART_RX = 48;
static const uint32_t UART_BAUD = 1000000;  // 1 Mbaud

// --- §9 framing ---
static const uint8_t SOF0 = 0xAA, SOF1 = 0x55;
static const uint8_t TYPE_HID_MOUSE = 0x01;
static const uint8_t MOUSE_EP = 0x81;

static unsigned long g_frames = 0;

static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
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

    const uint8_t *d = transfer->data_buffer;
    uint8_t  buttons = d[0];
    int16_t  dx = (int16_t)(d[2] | (d[3] << 8));
    int16_t  dy = (int16_t)(d[4] | (d[5] << 8));
    int8_t   wheel  = (int8_t)d[6];
    int8_t   hwheel = (int8_t)d[7];

    sendMouseFrame(buttons, dx, dy, wheel, hwheel);
    g_frames++;
    digitalWrite(PIN_LED_YELLOW, !digitalRead(PIN_LED_YELLOW));  // traffic blink
  }

  void onGone(const usb_host_client_event_msg_t *) override {
    Serial.println("[esp32] dongle disconnected");
    digitalWrite(PIN_LED_YELLOW, LOW);
  }
};

MouseForwarder dongle;

void setup() {
  pinMode(PIN_BOOST_EN,    OUTPUT); digitalWrite(PIN_BOOST_EN,    LOW);
  pinMode(PIN_DEV_VBUS_EN, OUTPUT); digitalWrite(PIN_DEV_VBUS_EN, HIGH);
  pinMode(PIN_LIMIT_EN,    OUTPUT); digitalWrite(PIN_LIMIT_EN,    HIGH);
  pinMode(PIN_USB_SEL,     OUTPUT); digitalWrite(PIN_USB_SEL,     HIGH);
  pinMode(PIN_LED_GREEN,   OUTPUT); digitalWrite(PIN_LED_GREEN,   HIGH);
  pinMode(PIN_LED_YELLOW,  OUTPUT); digitalWrite(PIN_LED_YELLOW,  LOW);
  delay(200);

  Serial.begin(115200);                                   // debug console -> COMx
  Serial1.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);  // link to Pico
  delay(1500);
  Serial.println();
  Serial.println("===== Phase 2: Mouse Forwarder =====");
  Serial.println("Green LED on = host mode. Move the Superlight; yellow LED = traffic.");

  dongle.begin();
}

void loop() {
  dongle.task();

  // Throttled heartbeat so we can see frames flowing without flooding the hot path.
  static unsigned long last = 0;
  if (millis() - last > 1000) {
    last = millis();
    Serial.printf("[esp32] mouse frames sent: %lu\n", g_frames);
  }
}
