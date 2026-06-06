// esp32_host.ino — Phase 1: dump descriptors + raw HID reports
//
// Hardware: ESP32-S3-USB-OTG (official Espressif dev board).
// Role: USB HOST. We power & enumerate the Logitech Lightspeed dongle on the
//       USB_HOST (Type-A female) port, then print every USB descriptor and
//       every raw HID report we receive to the UART (CP210x bridge -> COMx).
//
// READ THIS if nothing enumerates:
//   - The micro-USB port ALONE does not power the USB_HOST port. You MUST plug
//     the USB_DEV edge connector into a 5V source (PC port / wall wart / power
//     bank). The GPIO config below then passes that 5V to the host port.
//   - Green LED on  = firmware running, host mode configured.
//   - Yellow LED toggles on every HID report = traffic is flowing.
//
// Serial goes to UART0 -> CP210x USB-UART bridge (the micro-USB port), NOT the
// native USB peripheral — the native USB PHY is busy doing host duty.

#include <EspUsbHost.h>

// --- ESP32-S3-USB-OTG board control pins (see HANDOFF.md §5) ---
static const int PIN_USB_SEL     = 18;  // HIGH = route ESP32 USB lines to USB_HOST connector
static const int PIN_DEV_VBUS_EN = 12;  // HIGH = pass USB_DEV's 5V through to USB_HOST
static const int PIN_BOOST_EN    = 13;  // LOW  = battery boost converter off
static const int PIN_LIMIT_EN    = 17;  // HIGH = enable 500mA current-limit IC on host port
static const int PIN_LED_GREEN   = 15;  // power/status indicator
static const int PIN_LED_YELLOW  = 16;  // traffic indicator (toggles per HID report)

// DongleDumper subclasses EspUsbHost and overrides the two hooks we care about:
//   onReceive  — fires for every IN transfer (HID report) from the device
//   onGone     — fires when the device is unplugged
// The base library already prints the device/config/HID *descriptors* in
// PCAP-text format during enumeration, so we don't have to do that ourselves.
class DongleDumper : public EspUsbHost {
public:
  void onReceive(const usb_transfer_t *transfer) override {
    // Blink yellow so the user gets a physical "data is moving" signal.
    digitalWrite(PIN_LED_YELLOW, !digitalRead(PIN_LED_YELLOW));

    // Print the raw report as space-separated hex with a length prefix.
    // decode_reports.py parses exactly this "[RAW N] xx xx ..." format.
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
  // Configure host-mode power routing BEFORE anything else so the host port
  // comes up cleanly when the dongle is inserted.
  pinMode(PIN_BOOST_EN,    OUTPUT); digitalWrite(PIN_BOOST_EN,    LOW);
  pinMode(PIN_DEV_VBUS_EN, OUTPUT); digitalWrite(PIN_DEV_VBUS_EN, HIGH);
  pinMode(PIN_LIMIT_EN,    OUTPUT); digitalWrite(PIN_LIMIT_EN,    HIGH);
  pinMode(PIN_USB_SEL,     OUTPUT); digitalWrite(PIN_USB_SEL,     HIGH);
  pinMode(PIN_LED_GREEN,   OUTPUT); digitalWrite(PIN_LED_GREEN,   HIGH);
  pinMode(PIN_LED_YELLOW,  OUTPUT); digitalWrite(PIN_LED_YELLOW,  LOW);
  delay(200);

  Serial.begin(115200);
  delay(2000);  // give the USB-UART bridge time to re-enumerate on the PC
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
