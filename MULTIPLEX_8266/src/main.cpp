#include <Arduino.h>
#include <SoftwareSerial.h>

// GPIO4 (D2) and GPIO5 (D1) each drive a MOSFET switch module directly --
// no more PCF8574 I2C expander / multiplexer. The two channels alternate:
// one is on while the other is off.
static const uint8_t MOSFET_PIN_A = 4;
static const uint8_t MOSFET_PIN_B = 5;

// Third MOSFET channel, in phase with A (on/off together). GPIO0 is a boot
// strapping pin (must read HIGH at power-on for normal boot), but that only
// matters during reset -- driving it after setup() is safe.
static const uint8_t MOSFET_PIN_C = 0; // D3

// Fourth MOSFET channel, in phase with B (on/off together) -- so {A, C} and
// {B, D} form two alternating groups. GPIO15 is also a boot strapping pin
// (must read LOW at power-on for normal boot, opposite of GPIO0), same
// after-setup()-only caveat applies.
static const uint8_t MOSFET_PIN_D = 15; // D8

// Link to a second ESP8266 ("extender" board, see MOSFET_EXTENDER_8266) that
// drives two more MOSFET channels. Runs on SoftwareSerial (not the hardware
// UART) so USB debug output and the '1'/'0' active-low toggle below keep
// working over the USB cable. GPIO12/14 (D6/D5) are safe pins with no boot
// strapping behavior. Wire crossed: this TX (D5) -> extender RX (D6), this
// RX (D6) -> extender TX (D5), plus a shared GND between the two boards.
static const uint8_t LINK_RX_PIN = 12; // D6
static const uint8_t LINK_TX_PIN = 14; // D5
static const uint32_t LINK_BAUD = 9600;
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN);

// Frame sent to the extender board: [0xAA sync][cmd]. cmd bit0 = state
// (1=on, 0=off), bit1 = channel (0 or 1). The sync byte lets the extender
// resync after noise/garbage instead of misreading a stray byte as a command.
static const uint8_t LINK_SYNC_BYTE = 0xAA;

void sendLinkCommand(uint8_t channel, bool on) {
  uint8_t cmd = (channel << 1) | (on ? 1 : 0);
  linkSerial.write(LINK_SYNC_BYTE);
  linkSerial.write(cmd);
}

// The extender sends a heartbeat byte every ~250ms independent of command
// traffic (see MOSFET_EXTENDER_8266). We don't care what arrives, only that
// something does. Starts in WAITING rather than assuming "down", so the
// first LINK_TIMEOUT_MS after boot (while the extender is still starting
// up) doesn't immediately read the same as a real disconnect -- but WAITING
// still times out into a logged DOWN if nothing ever arrives, so silence
// always ends up reported instead of never being logged at all. While DOWN,
// we're still listening for the extender to show back up, so keep
// re-announcing every LINK_STATUS_REPEAT_MS instead of logging once and
// going quiet.
static const unsigned long LINK_TIMEOUT_MS = 1000;
static const unsigned long LINK_STATUS_REPEAT_MS = 2000;
enum LinkState { LINK_WAITING, LINK_UP, LINK_DOWN };
static LinkState linkState = LINK_WAITING;
static unsigned long linkStateSinceMillis = 0; // set in setup()
static unsigned long lastLinkRxMillis = 0;
static unsigned long lastLinkStatusPrintMillis = 0;

void updateLinkStatus() {
  bool gotByte = false;
  while (linkSerial.available()) {
    linkSerial.read();
    lastLinkRxMillis = millis();
    gotByte = true;
  }

  if (gotByte) {
    if (linkState != LINK_UP) {
      linkState = LINK_UP;
      Serial.println("[LINK] Extender connected");
    }
    return;
  }

  unsigned long referenceMillis = (linkState == LINK_WAITING) ? linkStateSinceMillis : lastLinkRxMillis;
  unsigned long downForMs = millis() - referenceMillis;
  if (downForMs < LINK_TIMEOUT_MS) {
    return;
  }

  bool justWentDown = (linkState != LINK_DOWN);
  if (justWentDown || millis() - lastLinkStatusPrintMillis >= LINK_STATUS_REPEAT_MS) {
    linkState = LINK_DOWN;
    lastLinkStatusPrintMillis = millis();
    Serial.printf("[LINK] Extender disconnected (no heartbeat for %lus), listening for reconnect...\n", downForMs / 1000);
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

// Same as delay(), but keeps polling serial so a polarity change lands within ~10ms
// instead of waiting out the rest of the alternation period.
void delayWithSerial(unsigned long ms) {
  unsigned long start = millis();
  do {
    handleSerial();
    updateLinkStatus();
    delay(10);
  } while (millis() - start < ms);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nMULTIPLEX_8266 starting...");
  Serial.println("Send '1' for active-LOW, '0' for active-HIGH (default).");

  bootFlashStatusLed();

  linkSerial.begin(LINK_BAUD);
  linkStateSinceMillis = millis();

  pinMode(MOSFET_PIN_A, OUTPUT);
  pinMode(MOSFET_PIN_B, OUTPUT);
  pinMode(MOSFET_PIN_C, OUTPUT);
  pinMode(MOSFET_PIN_D, OUTPUT);
  setMosfet(MOSFET_PIN_A, false); // off on boot
  setMosfet(MOSFET_PIN_B, false); // off on boot
  setMosfet(MOSFET_PIN_C, false); // off on boot
  setMosfet(MOSFET_PIN_D, false); // off on boot
  sendLinkCommand(0, false);
  sendLinkCommand(1, false);
}

void loop() {
  setMosfet(MOSFET_PIN_A, true);
  setMosfet(MOSFET_PIN_B, false);
  setMosfet(MOSFET_PIN_C, true);
  setMosfet(MOSFET_PIN_D, false);
  sendLinkCommand(0, true);
  sendLinkCommand(1, false);
  delayWithSerial(500);

  setMosfet(MOSFET_PIN_A, false);
  setMosfet(MOSFET_PIN_B, true);
  setMosfet(MOSFET_PIN_C, false);
  setMosfet(MOSFET_PIN_D, true);
  sendLinkCommand(0, false);
  sendLinkCommand(1, true);
  delayWithSerial(500);
}
