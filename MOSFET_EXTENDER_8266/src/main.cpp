#include <Arduino.h>
#include <SoftwareSerial.h>

// GPIO extender board: takes commands over UART from the primary
// (MULTIPLEX_8266) and drives two more MOSFET switch modules directly.
static const uint8_t MOSFET_PIN_A = 4; // channel 0
static const uint8_t MOSFET_PIN_B = 5; // channel 1

// Link to the primary board. Runs on SoftwareSerial (not the hardware UART)
// so USB debug output and the '1'/'0' active-low toggle below keep working
// over the USB cable. GPIO12/14 (D6/D5) are safe pins with no boot
// strapping behavior. Wire crossed: primary TX (D5) -> this RX (D6),
// primary RX (D6) -> this TX (D5), plus a shared GND between the two boards.
static const uint8_t LINK_RX_PIN = 12; // D6
static const uint8_t LINK_TX_PIN = 14; // D5
static const uint32_t LINK_BAUD = 9600;
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN);

// Frame from the primary: [0xAA sync][cmd]. cmd bit0 = state (1=on, 0=off),
// bit1 = channel (0 or 1). The sync byte lets us resync after noise/garbage
// instead of misreading a stray byte as a command.
static const uint8_t LINK_SYNC_BYTE = 0xAA;
static bool linkSynced = false;

// Sent back to the primary on its own schedule (independent of command
// traffic) so its logs can tell whether this board is actually connected,
// not just assume it. Content doesn't matter -- the primary only cares that
// bytes keep arriving.
static const unsigned long HEARTBEAT_INTERVAL_MS = 250;
static unsigned long lastHeartbeatMillis = 0;

void sendHeartbeatIfDue() {
  unsigned long now = millis();
  if (now - lastHeartbeatMillis >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMillis = now;
    linkSerial.write(LINK_SYNC_BYTE);
  }
}

// Onboard ESP-12E LED (D4), active-LOW. Flashes rapidly for 1s on bootup,
// then stays lit for the rest of runtime as a power/alive indicator.
static const uint8_t STATUS_LED_PIN = 2;

void bootFlashStatusLed() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  unsigned long start = millis();
  bool on = false;
  while (millis() - start < 1000) {
    on = !on;
    digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
    delay(50);
  }
  digitalWrite(STATUS_LED_PIN, LOW); // stays lit
}

// Bare GPIO driving MOSFET switch modules (not purpose-built active-low
// relay boards), so the standard wiring is gate-driven-HIGH = ON.
// Toggle live over serial instead of reflashing: send '1' for active-LOW,
// '0' for active-HIGH (default).
static bool activeLow = false;

void setMosfet(uint8_t pin, bool on) {
  bool level = activeLow ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
  Serial.printf("MOSFET[%u] %s\n", pin, on ? "ON" : "off");
}

void handleLinkCommand(uint8_t cmd) {
  uint8_t channel = (cmd >> 1) & 0x1;
  bool on = cmd & 0x1;
  uint8_t pin = (channel == 0) ? MOSFET_PIN_A : MOSFET_PIN_B;
  setMosfet(pin, on);
}

// Reads whatever bytes are waiting on the link and applies any complete
// [sync][cmd] frames. Runs a two-state parser instead of blocking on two
// reads at once so a sync byte that hasn't arrived yet never stalls loop().
void handleLink() {
  while (linkSerial.available()) {
    uint8_t b = linkSerial.read();
    if (!linkSynced) {
      if (b == LINK_SYNC_BYTE) {
        linkSynced = true;
      }
    } else {
      handleLinkCommand(b);
      linkSynced = false;
    }
  }
}

// Send '1' for active-LOW, '0' for active-HIGH; anything else (line endings, etc.) is ignored.
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '1' && !activeLow) {
      activeLow = true;
      Serial.println("ACTIVE_LOW = true");
    } else if (c == '0' && activeLow) {
      activeLow = false;
      Serial.println("ACTIVE_LOW = false");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nMOSFET_EXTENDER_8266 starting...");
  Serial.println("Send '1' for active-LOW, '0' for active-HIGH (default).");

  bootFlashStatusLed();

  linkSerial.begin(LINK_BAUD);

  pinMode(MOSFET_PIN_A, OUTPUT);
  pinMode(MOSFET_PIN_B, OUTPUT);
  setMosfet(MOSFET_PIN_A, false); // off until the primary says otherwise
  setMosfet(MOSFET_PIN_B, false);
}

void loop() {
  handleLink();
  handleSerial();
  sendHeartbeatIfDue();
}
