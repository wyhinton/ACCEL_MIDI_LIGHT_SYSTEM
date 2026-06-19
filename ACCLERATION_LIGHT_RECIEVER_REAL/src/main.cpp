/*
  Impact Light – RECEIVER (ESP32-S3 / Seeed XIAO ESP32-S3)

  Listens for ESP-NOW "impact" messages broadcast by the sender board
  (ACCELERATION_LIGHT_SENDER_REAL). Every time an impact arrives, it
  flashes a light on/off by pulsing a relay.

  Wiring:
    RELAY_PIN -> relay module IN
    relay COM/NO -> the light + its power supply
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// -------- IMPACT MESSAGE --------
// MUST match the sender's struct byte-for-byte, or the data is garbage.
typedef struct __attribute__((packed)) {
  uint32_t seq;       // increments each impact
  float    jerk;      // measured jerk (g/frame) that triggered it
  float    accelMag;  // acceleration magnitude at impact (g)
} ImpactMessage;

// -------- HANDSHAKE / LINK STATUS --------
// Mirrors the sender: it beacons HELLO, we reply ACK. Both boards then know
// the link works. Distinguished from ImpactMessage by length (1 vs 12 bytes).
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2 };
typedef struct __attribute__((packed)) {
  uint8_t type;
} HandshakeMessage;

// -------- FLASH COMMAND (from the MIDI board) --------
// Velocity-scaled flash request. The MIDI board does the velocity->brightness
// /duration mapping (single source of truth for the scaling rules) and sends
// the resolved values here. Distinguished from the others by length:
//   HandshakeMessage = 1 byte, FlashCommand = 5 bytes, ImpactMessage = 12 bytes.
typedef struct __attribute__((packed)) {
  uint8_t  cmd;         // = FLASH_CMD_MAGIC
  uint8_t  velocity;    // original MIDI velocity 1..127 (for logging)
  uint8_t  brightness;  // matrix brightness 0..255 (already scaled)
  uint16_t durationMs;  // flash duration in ms (already scaled)
} FlashCommand;
#define FLASH_CMD_MAGIC 0xF1

// Broadcast back so the sender hears the ACK. esp_now_send needs a registered
// peer even for broadcast.
uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

const unsigned long LINK_TIMEOUT_MS       = 3000; // no HELLO this long => link down
const unsigned long RED_BLINK_INTERVAL_MS = 3000; // how often to warn when down

volatile unsigned long lastHelloMs = 0;   // last HELLO from sender (set in recv cb)
volatile bool          needAck     = false;
bool          linkUp         = false;
unsigned long lastRedBlinkMs = 0;

// -------- RELAY / LIGHT --------
#define RELAY_PIN        2     // GPIO driving the relay IN pin
#define RELAY_ACTIVE_HIGH true // true: HIGH = relay ON. Set false for active-low modules.
#define FLASH_DURATION_MS 150  // default flash length (impact msgs / fallback)

// -------- LED MATRIX --------
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

// Non-blocking flash state. The receive callback just records that a flash
// was requested; loop() turns the relay off once the duration elapses.
volatile bool          flashActive    = false;
volatile unsigned long flashStartMs   = 0;
volatile unsigned long flashDurationMs = FLASH_DURATION_MS; // set per request
volatile uint8_t       flashBrightness = 255;               // set per request
volatile uint8_t       flashVelocity  = 0;                  // for logging
volatile uint32_t      lastSeq        = 0;
volatile bool          newImpact      = false;

void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

// ESP-NOW receive callback. Signature differs across Arduino-ESP32 cores.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  // Handshake beacon from the sender: note it and queue an ACK reply.
  if (len == sizeof(HandshakeMessage)) {
    HandshakeMessage hs;
    memcpy(&hs, data, sizeof(hs));
    if (hs.type == HS_HELLO) {
      lastHelloMs = millis();
      needAck     = true;   // reply from loop(), not here
    }
    return;
  }

  // Velocity-scaled flash command from the MIDI board.
  if (len == sizeof(FlashCommand)) {
    FlashCommand fc;
    memcpy(&fc, data, sizeof(fc));
    if (fc.cmd != FLASH_CMD_MAGIC) return;   // not ours

    flashBrightness = fc.brightness;
    flashDurationMs = fc.durationMs ? fc.durationMs : FLASH_DURATION_MS;
    flashVelocity   = fc.velocity;
    flashStartMs    = millis();
    flashActive     = true;
    newImpact       = true;
    return;
  }

  if (len != sizeof(ImpactMessage)) {
    // Not our message format – ignore.
    return;
  }

  ImpactMessage msg;
  memcpy(&msg, data, sizeof(msg));

  // Start (or restart) the flash. Keep callback work minimal — the relay
  // and matrix are driven from loop() to avoid heavy work in this context.
  // Impacts use the default full-brightness, fixed-duration flash.
  lastSeq         = msg.seq;
  flashBrightness = 255;
  flashDurationMs = FLASH_DURATION_MS;
  flashVelocity   = 0;
  flashStartMs    = millis();
  flashActive     = true;
  newImpact       = true;
}

void flashOn(uint8_t brightness) {
  relayWrite(true);   // relay is on/off only; brightness applies to the matrix
  matrix.setBrightness(brightness);
  matrix.fillScreen(matrix.Color(255, 255, 255));
  matrix.show();
}

void flashOff() {
  relayWrite(false);
  matrix.fillScreen(0);
  matrix.show();
}

void sendAck() {
  HandshakeMessage hs;
  hs.type = HS_ACK;
  esp_now_send(broadcastAddr, (const uint8_t *)&hs, sizeof(hs));
}

// Brief blocking blink of the whole matrix in one color.
void blinkMatrix(uint8_t r, uint8_t g, uint8_t b, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    matrix.setBrightness(120);
    matrix.fillScreen(matrix.Color(r, g, b));
    matrix.show();
    delay(onMs);
    matrix.fillScreen(0);
    matrix.show();
    delay(offMs);
  }
}

// Show a single identity letter on the matrix for a moment (startup banner).
// The built-in 5x7 GFX font fits one character in the 8x8 grid.
void showStartupLetter(char c, uint8_t r, uint8_t g, uint8_t b, int holdMs) {
  matrix.setBrightness(120);
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);
  matrix.setTextColor(matrix.Color(r, g, b));
  matrix.setCursor(2, 1);   // roughly center the 5x7 glyph in the 8x8 grid
  matrix.print(c);
  matrix.show();
  delay(holdMs);
  matrix.fillScreen(0);
  matrix.show();
}

// Show link status: green burst the instant a HELLO link comes up, occasional
// red while no HELLO has been heard recently.
void updateLinkStatus() {
  unsigned long now = millis();
  bool connectedNow = (lastHelloMs != 0) && (now - lastHelloMs < LINK_TIMEOUT_MS);

  if (connectedNow && !linkUp) {
    linkUp = true;
    Serial.println("LINK UP (HELLO received)");
    blinkMatrix(0, 255, 0, 4, 150, 120);   // green: handshake OK
  } else if (!connectedNow) {
    if (linkUp) {
      linkUp = false;
      Serial.println("LINK DOWN (no HELLO)");
    }
    if (now - lastRedBlinkMs >= RED_BLINK_INTERVAL_MS) {
      lastRedBlinkMs = now;
      blinkMatrix(255, 0, 0, 1, 150, 0);   // red: no link yet
    }
  }
}

void initEspNow() {
  // ESP-NOW rides on the Wi-Fi radio; station mode, not connected to any AP.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("Receiver STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }

  esp_now_register_recv_cb(onEspNowRecv);

  // Register the broadcast peer so we can send ACK replies.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, 6);
  peer.channel = 0;       // use the current Wi-Fi channel
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add ESP-NOW peer!");
  }

  Serial.println("ESP-NOW ready – waiting for impact messages.");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);   // light off at boot

  matrix.begin();
  matrix.setBrightness(255);
  matrix.fillScreen(0);
  matrix.show();

  // Startup banner: 'R' identifies this board as the RECEIVER.
  showStartupLetter('R', 0, 0, 255, 1000);

  initEspNow();
}

void loop() {
  // Reply to the sender's handshake beacon (queued by the recv callback).
  if (needAck) {
    needAck = false;
    sendAck();
  }

  // New flash request: turn relay + LEDs on, and log (Serial/matrix in the
  // ESP-NOW callback is risky, so we do it here).
  if (newImpact) {
    newImpact = false;
    flashOn(flashBrightness);
    if (flashVelocity > 0) {
      Serial.print("FLASH (MIDI)  vel="); Serial.print(flashVelocity);
      Serial.print(" bright=");           Serial.print(flashBrightness);
      Serial.print(" dur=");              Serial.print(flashDurationMs);
      Serial.println("ms");
    } else {
      Serial.print("IMPACT received  seq="); Serial.println(lastSeq);
    }
  }

  // Turn the light + LEDs back off once the flash duration has elapsed.
  if (flashActive && (millis() - flashStartMs >= flashDurationMs)) {
    flashActive = false;
    flashOff();
  }

  // Connection status LEDs — only when not mid impact-flash, so they don't
  // fight over the matrix.
  if (!flashActive) {
    updateLinkStatus();
  }
}
