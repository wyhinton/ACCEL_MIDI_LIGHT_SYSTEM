// Pin map:
//   GPIO4  (D2) - I2C SDA (PCA9685 + OLED)
//   GPIO5  (D1) - I2C SCL (PCA9685 + OLED)
//   GPIO2  (D4) - onboard status LED, active-LOW (see STATUS_LED_PIN)
//
// Control: this board is 8 extra Art-Net channels for the MULTIPLEX_8266
// fixture (its channels 8-15), driven over PWM by the PCA9685. It doesn't
// speak Art-Net itself, and there's no wire to MULTIPLEX_8266 either: this
// board joins MULTIPLEX_8266's own SoftAP as a Wi-Fi station (the same
// open network QLC+ joins for Art-Net), and the two exchange
// [0xAC sync][channel 0-7][duty 0-255] duty commands and heartbeat bytes as
// UDP datagrams instead of serial bytes -- the same framing MULTIPLEX_8266
// already uses for MOSFET_EXTENDER_8266's wired channels. See
// MULTIPLEX_8266's ARTNET_CONTROL.md for how QLC+ addresses the combined
// fixture.

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

#define SDA_PIN 4   // ESP8266 D2
#define SCL_PIN 5   // ESP8266 D1

#define NUM_CHANNELS 16

#define OLED_WIDTH 128
#define OLED_HEIGHT 32
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// Last commanded PWM value (0-4095) per channel, used to draw the level bars.
uint16_t channelLevels[NUM_CHANNELS] = {0};

// Link to MULTIPLEX_8266 over Wi-Fi: this board joins its SoftAP as a
// station and exchanges UDP datagrams with it instead of a serial wire.
// MULTIPLEX_8266's AP always hands out 192.168.4.1 to itself (see its
// ARTNET_CONTROL.md), so that address is fixed even though this board's own
// address (assigned by the AP's DHCP server) isn't.
static const char *WIFI_SSID = "MULTIPLEX_LIGHTS"; // open network, no password
static const IPAddress MULTIPLEX_AP_IP(192, 168, 4, 1);
static const uint16_t RELAY_HEARTBEAT_PORT = 7778; // this board -> MULTIPLEX_8266
static const uint16_t RELAY_DUTY_PORT = 7779;      // MULTIPLEX_8266 -> this board
static const uint8_t LINK_CHANNEL_COUNT = 8;
WiFiUDP linkUdp;

// [0xAC sync][channel 0-7][duty 0-255], one per UDP packet -- streamed
// continuously by the primary's effects fade engine as each channel ramps.
// UDP already frames the packet, so unlike a byte-stream link there's no
// resync state machine needed: a short or malformed packet is just dropped.
static const uint8_t LINK_DUTY_SYNC_BYTE = 0xAC;

// Sent back to the primary on its own schedule (independent of command
// traffic) so its logs can tell whether this board is actually connected,
// not just assume it. Content doesn't matter -- the primary only cares that
// packets keep arriving.
static const unsigned long LINK_HEARTBEAT_INTERVAL_MS = 250;
static unsigned long lastLinkHeartbeatMillis = 0;

void sendLinkHeartbeatIfDue() {
  if (WiFi.status() != WL_CONNECTED) {
    return; // nothing to say until we're actually on the network
  }
  if (millis() - lastLinkHeartbeatMillis >= LINK_HEARTBEAT_INTERVAL_MS) {
    lastLinkHeartbeatMillis = millis();
    linkUdp.beginPacket(MULTIPLEX_AP_IP, RELAY_HEARTBEAT_PORT);
    linkUdp.write(LINK_DUTY_SYNC_BYTE);
    linkUdp.endPacket();
  }
}

// Logs Wi-Fi connect/disconnect transitions to USB serial (this board has
// no telnet debug mirror like MULTIPLEX_8266) -- the core's STA mode
// reconnects on its own, this just narrates when that happens.
static bool wifiWasConnected = false;

void logWifiStatusChange() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected == wifiWasConnected) {
    return;
  }
  wifiWasConnected = connected;
  if (connected) {
    Serial.printf("[WIFI] connected to %s, IP %s\n", WIFI_SSID, WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("[WIFI] disconnected -- retrying..."));
  }
}

// Onboard ESP-12E LED (D4/GPIO2), active-LOW. Blinks on a fixed interval as
// a simple "firmware is alive" heartbeat, independent of the fade timing.
static const uint8_t STATUS_LED_PIN = LED_BUILTIN;
static const unsigned long HEARTBEAT_INTERVAL_MS = 500;
unsigned long lastHeartbeatToggle = 0;
bool heartbeatOn = false;

// Throttle OLED redraws so the shared I2C bus isn't saturated by display
// writes on every fade step.
static const unsigned long DISPLAY_INTERVAL_MS = 50;
unsigned long lastDisplayUpdate = 0;

void updateHeartbeat() {
  if (millis() - lastHeartbeatToggle >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatToggle = millis();
    heartbeatOn = !heartbeatOn;
    digitalWrite(STATUS_LED_PIN, heartbeatOn ? LOW : HIGH);
  }
}

void setChannel(uint8_t channel, uint16_t brightness) {
  pwm.setPWM(channel, 0, brightness);
  channelLevels[channel] = brightness;
}

// Applies one [0xAC][channel][duty] frame's raw duty (0-255) to the PCA9685,
// scaled up to its 0-4095 PWM range.
void handleLinkChannelDuty(uint8_t channel, uint8_t duty8) {
  uint16_t brightness = (uint16_t)((uint32_t)duty8 * 4095 / 255);
  setChannel(channel, brightness);
}

// Applies every complete [0xAC][channel 0-7][duty 0-255] packet waiting on
// the link. A packet that's too short, doesn't start with the sync byte, or
// names a channel we don't have is just dropped -- UDP already guarantees
// each read is one whole datagram (or none), so there's no partial-frame
// state to track like a byte-stream link would need.
void pollLink() {
  while (linkUdp.parsePacket() > 0) {
    uint8_t buf[3];
    int len = linkUdp.read(buf, sizeof(buf));
    if (len < 3 || buf[0] != LINK_DUTY_SYNC_BYTE || buf[1] >= LINK_CHANNEL_COUNT) {
      continue;
    }
    handleLinkChannelDuty(buf[1], buf[2]);
  }
}

// One-shot boot diagnostic: lists every I2C address that ACKs, so wiring /
// address problems (e.g. OLED not actually at OLED_ADDR) show up in the
// Serial Monitor instead of as a silently-failed display.begin().
void scanI2CBus() {
  Serial.println(F("Scanning I2C bus..."));
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println(F("  no I2C devices found - check wiring/power"));
  }
}

// Same as delay(), but keeps servicing the link so a long pause here (e.g.
// the boot splash below) doesn't drop incoming duty packets or let our own
// heartbeat go quiet long enough for the primary to log us as down.
void delayWhilePollingLink(unsigned long ms) {
  unsigned long start = millis();
  do {
    pollLink();
    sendLinkHeartbeatIfDue();
    delay(10);
  } while (millis() - start < ms);
}

void updateDisplay() {
  if (millis() - lastDisplayUpdate < DISPLAY_INTERVAL_MS) {
    return;
  }
  lastDisplayUpdate = millis();

  const uint8_t barPitch = OLED_WIDTH / NUM_CHANNELS; // 8px per channel
  const uint8_t barWidth = barPitch - 1;              // 1px gap between bars

  display.clearDisplay();
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    uint8_t barHeight = map(channelLevels[ch], 0, 4095, 0, OLED_HEIGHT);
    int16_t x = ch * barPitch;
    int16_t y = OLED_HEIGHT - barHeight;
    display.fillRect(x, y, barWidth, barHeight, SSD1306_WHITE);
  }
  display.display();
}

void setup() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH); // off (active-LOW)

  Serial.begin(115200);
  delay(200);
  Serial.println(F("\nRELAY_PWM_8266 booting..."));
  Serial.println(F("8 extra Art-Net channels for MULTIPLEX_8266 -- see its ARTNET_CONTROL.md"));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID); // open network; the core retries/reconnects on its own
  linkUdp.begin(RELAY_DUTY_PORT);
  Serial.printf("[WIFI] joining %s...\n", WIFI_SSID);

  Wire.begin(SDA_PIN, SCL_PIN);
  // Conservative I2C speed: the SSD1306 lib defaults to 400kHz, which can be
  // too fast for the ESP8266's soft SDA/SCL pull-ups once a second device
  // (the PCA9685) shares the bus, causing corrupted/garbled display frames.
  Wire.setClock(100000);

  scanI2CBus();

  pwm.begin();
  pwm.setOscillatorFrequency(25000000);
  pwm.setPWMFreq(1000);  // 1 kHz PWM
 
  // Start all outputs off
  for (int i = 0; i < NUM_CHANNELS; i++) {
    setChannel(i, 0);
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 init FAILED - check OLED_ADDR and wiring"));
    // Distinct fast blink (vs the 500ms heartbeat) signals init failure even
    // without a Serial Monitor attached.
    while (true) {
      digitalWrite(STATUS_LED_PIN, LOW);
      delay(100);
      digitalWrite(STATUS_LED_PIN, HIGH);
      delay(100);
    }
  }
  Serial.println(F("SSD1306 init OK"));

  // Static text held on screen for a few seconds as a signal-integrity
  // sanity check: if this glitches too, the problem is the I2C link itself;
  // if this stays clean but the bar graph glitches, the problem is specific
  // to the redraw loop (rate, or contention with the PCA9685 writes).
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(40, 8);
  display.print(F("TEST"));
  display.display();
  delayWhilePollingLink(5000);

  display.clearDisplay();
  display.display();
}

// Control is entirely event-driven (pollLink() applies each channel's duty
// as soon as its frame arrives), so loop() just needs to keep the link,
// heartbeat, and display polling alive. There's no timeout hand-back --
// losing the link leaves the PCA9685 outputs holding their last commanded
// state, same as MULTIPLEX_8266's own Art-Net and extender-link behavior.
void loop() {
  logWifiStatusChange();
  pollLink();
  sendLinkHeartbeatIfDue();
  updateHeartbeat();
  updateDisplay();
}
