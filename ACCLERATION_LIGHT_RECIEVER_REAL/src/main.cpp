/*
  Soft Random – RECEIVER (ESP32-S3 / Seeed XIAO ESP32-S3)

  Impact/crash handling has been removed. This board now glows on its own: the
  8×8 RGB matrix pulses smoothly and randomly, and the relay-driven light
  follows that same wave (a relay can't dim, so it switches on/off with
  hysteresis as the pulse rises and falls — a gentle, slow cycle). MIDI flashes
  from the MIDI board (over ESP-NOW) still pop ON TOP: relay forced on, matrix
  to the flash brightness for the flash duration.

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

// -------- HANDSHAKE / LINK STATUS --------
// Mirrors the sender: it beacons HELLO, we reply ACK. Distinguished from the
// FlashCommand by length (1 byte vs 5 bytes).
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2 };
typedef struct __attribute__((packed)) {
  uint8_t type;
} HandshakeMessage;

// -------- FLASH COMMAND (from the MIDI board) --------
// Velocity-scaled flash request. The MIDI board does the velocity->brightness
// /duration mapping and sends the resolved values here.
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
#define RELAY_PIN         2     // GPIO driving the relay IN pin
#define RELAY_ACTIVE_HIGH true  // true: HIGH = relay ON. Set false for active-low modules.
#define FLASH_DURATION_MS 150   // default MIDI-flash length / fallback

// Relay can't dim, so it tracks the pulse with hysteresis: turns ON when the
// pulse rises past HIGH, OFF when it falls below LOW. The gap keeps it from
// chattering around a single threshold.
const uint8_t RELAY_ON_THRESHOLD  = 170;
const uint8_t RELAY_OFF_THRESHOLD = 90;

// -------- LED MATRIX --------
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

// Matrix is bright at close range, so the pulse is scaled to this ceiling.
const uint8_t MATRIX_MAX_BRIGHTNESS = 90;

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

// -------- SMOOTH RANDOM PULSE --------
// Brightness wanders smoothly and randomly: ease from the current level toward
// a new random target over a random duration, then pick another. smoothstep
// easing keeps the motion gentle, with no hard corners.
struct SmoothRandomPulse {
  float    fromVal  = 0.0f;
  float    toVal    = 0.0f;
  uint32_t segStart = 0;
  uint32_t segDur   = 1;
  uint8_t  minVal   = 0,   maxVal   = 255;
  uint16_t minDurMs = 600, maxDurMs = 2500;

  void begin(uint8_t lo, uint8_t hi, uint16_t durLo, uint16_t durHi) {
    minVal = lo; maxVal = hi; minDurMs = durLo; maxDurMs = durHi;
    fromVal = toVal = lo;
    segStart = millis();
    segDur   = 1;   // expire immediately so the first value() picks a target
  }

  uint8_t value(uint32_t now) {
    uint32_t elapsed = now - segStart;
    if (elapsed >= segDur) {
      fromVal  = toVal;
      toVal    = random(minVal, maxVal + 1);
      segDur   = random(minDurMs, maxDurMs + 1);
      segStart = now;
      elapsed  = 0;
    }
    float t = (float)elapsed / (float)segDur;     // 0..1
    float e = t * t * (3.0f - 2.0f * t);          // smoothstep
    return (uint8_t)(fromVal + (toVal - fromVal) * e);
  }
};

SmoothRandomPulse pulse;
bool relayOn = false;

// MIDI flash state (set in the recv callback, rendered in loop()).
volatile bool          flashActive     = false;
volatile unsigned long flashStartMs    = 0;
volatile unsigned long flashDurationMs = FLASH_DURATION_MS;
volatile uint8_t       flashBrightness = 255;
volatile uint8_t       flashVelocity   = 0;
volatile bool          newFlash        = false;

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
    newFlash        = true;
    return;
  }
}

void sendAck() {
  HandshakeMessage hs;
  hs.type = HS_ACK;
  esp_now_send(broadcastAddr, (const uint8_t *)&hs, sizeof(hs));
}

// Brief blocking blink of the whole matrix in one color (link status).
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

  Serial.println("ESP-NOW ready – link + MIDI flash enabled.");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // True-random seed so two boards don't pulse in lockstep.
  randomSeed(esp_random());

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);   // light off at boot

  matrix.begin();
  matrix.setBrightness(0);
  matrix.fillScreen(0);
  matrix.show();

  // Pulse 0..255 intensity, easing over 0.6–2.5 s segments.
  pulse.begin(0, 255, 600, 2500);

  initEspNow();

  Serial.println("Soft random pulse running.");
}

void loop() {
  unsigned long now = millis();

  // Reply to the sender's handshake beacon (queued by the recv callback).
  if (needAck) {
    needAck = false;
    sendAck();
  }

  if (newFlash) {
    newFlash = false;
    Serial.print("FLASH (MIDI)  vel="); Serial.print(flashVelocity);
    Serial.print(" bright=");           Serial.print(flashBrightness);
    Serial.print(" dur=");              Serial.print(flashDurationMs);
    Serial.println("ms");
  }

  // A blocking link blink would stall the pulse, so only run link status while
  // idle (no MIDI flash in flight) — matching the original guard.
  bool flashing = flashActive && (now - flashStartMs < flashDurationMs);
  if (!flashing) {
    flashActive = false;
    updateLinkStatus();
  }

  // ---- SMOOTH RANDOM PULSE (base layer) ----
  uint8_t pulseVal   = pulse.value(now);                                   // 0..255
  uint8_t matrixBase = (uint8_t)((uint16_t)pulseVal * MATRIX_MAX_BRIGHTNESS / 255);

  // Relay follows the pulse with hysteresis (it can only switch, not dim).
  if (!relayOn && pulseVal >= RELAY_ON_THRESHOLD)      relayOn = true;
  else if (relayOn && pulseVal <= RELAY_OFF_THRESHOLD) relayOn = false;

  // ---- RENDER: MIDI flash overrides on top, else the pulse ----
  if (flashing) {
    relayWrite(true);                       // relay forced on for the flash
    matrix.setBrightness(flashBrightness);
  } else {
    relayWrite(relayOn);
    matrix.setBrightness(matrixBase);
  }
  matrix.fillScreen(matrix.Color(255, 255, 255));
  matrix.show();
}
