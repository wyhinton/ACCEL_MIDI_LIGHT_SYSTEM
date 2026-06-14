/*
  Soft Random – SENDER (ESP32-S3 RGB LED Matrix, Waveshare)

  Crash/IMU detection has been removed. This board now just glows: the 8×8 RGB
  matrix and the PWM light on GPIO 2 pulse smoothly and randomly (brightness
  eases between random targets over random durations). MIDI-triggered flashes
  from the MIDI board (over ESP-NOW) still pop ON TOP of the pulse.
*/

#include <Arduino.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#include <WiFi.h>
#include <esp_now.h>

// -------- ESP-NOW PEER --------
// Broadcast: any ESP32 running an ESP-NOW peer on the same WiFi channel hears
// us. To target one board, replace with its STA MAC.
uint8_t espNowPeerMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

bool espNowReady = false;

// -------- HANDSHAKE / LINK STATUS --------
// The sender beacons HELLO; the receiver replies ACK. Distinguished from the
// FlashCommand by length (1 byte vs 5 bytes).
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2 };
typedef struct __attribute__((packed)) {
  uint8_t type;
} HandshakeMessage;

// -------- FLASH COMMAND (from the MIDI board) --------
// Velocity-scaled flash request broadcast by MIDI_NOTE_LIGHT_REAL. We layer the
// flash (at the given brightness) on top of the pulse for its duration.
typedef struct __attribute__((packed)) {
  uint8_t  cmd;         // = FLASH_CMD_MAGIC
  uint8_t  velocity;    // original MIDI velocity 1..127 (for logging)
  uint8_t  brightness;  // matrix brightness 0..255 (already scaled)
  uint16_t durationMs;  // flash duration in ms (already scaled)
} FlashCommand;
#define FLASH_CMD_MAGIC 0xF1

// -------- AUDIO LEVEL (from the Mac, via the MIDI board's BLE bridge) --------
// 2-byte message carrying a smoothed output-audio level (0..255) that scales
// the pulse/light brightness. Distinct from Handshake(1) and FlashCommand(5)
// by length. If none arrive for a while we fall back to full brightness so the
// lights never go dark with no Mac connected.
typedef struct __attribute__((packed)) {
  uint8_t cmd;    // = LEVEL_CMD_MAGIC
  uint8_t level;  // 0..255
} LevelMessage;
#define LEVEL_CMD_MAGIC 0xA1

const unsigned long LEVEL_TIMEOUT_MS = 1500;  // no level this long => full bright

volatile uint8_t       audioLevelRaw = 255;   // last level received (set in recv cb)
volatile unsigned long lastLevelMs   = 0;      // when it arrived
float                  audioLevelSmoothed = 255.0f;  // on-device EMA (bridges gaps)

// MIDI-triggered flash state (set in the recv callback, rendered in loop()).
volatile bool          midiFlashActive     = false;
volatile bool          newMidiFlash        = false;
volatile unsigned long midiFlashStartMs    = 0;
volatile unsigned long midiFlashDurationMs = 0;
volatile uint8_t       midiFlashBrightness = 255;
volatile uint8_t       midiFlashVelocity   = 0;

const unsigned long HELLO_INTERVAL_MS     = 1000;  // beacon period
const unsigned long LINK_TIMEOUT_MS       = 3000;  // no ACK this long => link down
const unsigned long RED_BLINK_INTERVAL_MS = 3000;  // how often to warn when down

volatile unsigned long lastAckMs = 0;   // last ACK arrival (set in recv cb)
bool          linkUp          = false;  // debounced link state
unsigned long lastHelloSentMs = 0;
unsigned long lastRedBlinkMs  = 0;

// Send-status callback (optional, for debugging delivery).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  // Quiet by default; uncomment to debug delivery.
  // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "send OK" : "send FAIL");
}

// Receive callback: listen for ACKs (link status) and MIDI flash commands.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(HandshakeMessage)) {
    HandshakeMessage hs;
    memcpy(&hs, data, sizeof(hs));
    if (hs.type == HS_ACK) {
      lastAckMs = millis();
    }
    return;
  }

  // Audio level from the Mac (relayed by the MIDI board's BLE bridge).
  if (len == sizeof(LevelMessage)) {
    LevelMessage lm;
    memcpy(&lm, data, sizeof(lm));
    if (lm.cmd != LEVEL_CMD_MAGIC) return;
    audioLevelRaw = lm.level;
    lastLevelMs   = millis();
    return;
  }

  // Velocity-scaled flash command from the MIDI board.
  if (len == sizeof(FlashCommand)) {
    FlashCommand fc;
    memcpy(&fc, data, sizeof(fc));
    if (fc.cmd != FLASH_CMD_MAGIC) return;
    midiFlashBrightness = fc.brightness;
    midiFlashDurationMs = fc.durationMs ? fc.durationMs : 100;
    midiFlashVelocity   = fc.velocity;
    midiFlashStartMs    = millis();
    midiFlashActive     = true;
    newMidiFlash        = true;
    return;
  }
}

void espNowBegin() {
  // ESP-NOW runs on the WiFi radio. STA mode, no AP connection needed.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("This board STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    return;
  }

  esp_now_register_send_cb(onEspNowSent);
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, espNowPeerMac, 6);
  peer.channel = 0;       // use the current WiFi channel
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW add peer FAILED");
    return;
  }

  espNowReady = true;
  Serial.println("ESP-NOW ready (link + MIDI flash enabled)");
}

void espNowSendHello() {
  if (!espNowReady) return;
  HandshakeMessage hs;
  hs.type = HS_HELLO;
  esp_now_send(espNowPeerMac, (const uint8_t *)&hs, sizeof(hs));
}

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

// -------- PWM LIGHT (GPIO 2) --------
#define LIGHT_PIN           2      // PWM-capable GPIO driving the light
#define LIGHT_PWM_FREQ      5000   // Hz
#define LIGHT_PWM_RES_BITS  8      // 8-bit duty -> 0..255
#define LIGHT_PWM_CHANNEL   0      // LEDC channel (core 2.x only)

void lightWriteDuty(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LIGHT_PIN, duty);
#else
  ledcWrite(LIGHT_PWM_CHANNEL, duty);
#endif
}

void lightBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LIGHT_PIN, LIGHT_PWM_FREQ, LIGHT_PWM_RES_BITS);
#else
  ledcSetup(LIGHT_PWM_CHANNEL, LIGHT_PWM_FREQ, LIGHT_PWM_RES_BITS);
  ledcAttachPin(LIGHT_PIN, LIGHT_PWM_CHANNEL);
#endif
  lightWriteDuty(0);
}

// -------- LED MATRIX SETUP --------
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

// Matrix is bright at close range, so the pulse is scaled down to this ceiling.
const uint8_t MATRIX_MAX_BRIGHTNESS = 90;

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

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

// Beacon HELLO, watch for ACK, and show link status on the matrix:
// a green burst the instant the link comes up, occasional red while it's down.
void updateLinkStatus() {
  unsigned long now = millis();

  if (now - lastHelloSentMs >= HELLO_INTERVAL_MS) {
    lastHelloSentMs = now;
    espNowSendHello();
  }

  bool connectedNow = (lastAckMs != 0) && (now - lastAckMs < LINK_TIMEOUT_MS);

  if (connectedNow && !linkUp) {
    linkUp = true;
    Serial.println("LINK UP (ACK received)");
    blinkMatrix(0, 255, 0, 4, 150, 120);   // green: handshake OK
  } else if (!connectedNow) {
    if (linkUp) {
      linkUp = false;
      Serial.println("LINK DOWN (no ACK)");
    }
    if (now - lastRedBlinkMs >= RED_BLINK_INTERVAL_MS) {
      lastRedBlinkMs = now;
      blinkMatrix(255, 0, 0, 1, 150, 0);   // red: no link yet
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // True-random seed so two boards don't pulse in lockstep.
  randomSeed(esp_random());

  lightBegin();   // PWM light, starts off

  matrix.begin();
  matrix.setBrightness(0);
  matrix.fillScreen(0);
  matrix.show();

  // Startup banner: 'S' identifies this board as the SENDER.
  showStartupLetter('S', 0, 0, 255, 1000);

  // Pulse 0..255 intensity, easing over 0.6–2.5 s segments.
  pulse.begin(0, 255, 600, 2500);

  espNowBegin();   // WiFi/ESP-NOW radio: link status + MIDI flash commands

  Serial.println("Soft random pulse running.");
}

void loop() {
  while (Serial.available()) Serial.read();   // drain unused serial input

  updateLinkStatus();           // beacon HELLO + show green/red link status

  unsigned long now = millis();

  // ---- MIDI FLASH ENVELOPE (from the MIDI board over ESP-NOW) ----
  // Decays linearly from the velocity-scaled peak back to 0 and is layered ON
  // TOP of the pulse (brighter of the two wins), so a note pops above the glow
  // and fades back into it. Recomputed every loop so quick notes retrigger.
  uint8_t flashBoost = 0;
  {
    unsigned long since = now - midiFlashStartMs;
    if (midiFlashActive && since < midiFlashDurationMs) {
      float env = 1.0f - (float)since / (float)midiFlashDurationMs; // 1 -> 0
      flashBoost = (uint8_t)(env * midiFlashBrightness);
    } else {
      midiFlashActive = false;
    }
  }
  if (newMidiFlash) {
    newMidiFlash = false;
    Serial.print("MIDI FLASH  vel="); Serial.print(midiFlashVelocity);
    Serial.print(" bright=");         Serial.print(midiFlashBrightness);
    Serial.print(" dur=");            Serial.print(midiFlashDurationMs);
    Serial.println("ms");
  }

  // ---- AUDIO-LEVEL MULTIPLIER ----
  // Track the Mac's streamed level with a light EMA so dropped packets don't
  // cause flicker; fall back to full brightness if the stream goes silent.
  uint8_t levelTarget = (lastLevelMs != 0 && now - lastLevelMs < LEVEL_TIMEOUT_MS)
                          ? audioLevelRaw : 255;
  audioLevelSmoothed += ((float)levelTarget - audioLevelSmoothed) * 0.2f;
  float audioMul = audioLevelSmoothed / 255.0f;   // 0..1

  // ---- SMOOTH RANDOM PULSE (base layer), scaled by the audio level ----
  uint8_t pulseVal   = (uint8_t)(pulse.value(now) * audioMul);      // 0..255
  uint8_t matrixBase = (uint8_t)((uint16_t)pulseVal * MATRIX_MAX_BRIGHTNESS / 255);

  // ---- RENDER MATRIX (every loop) ----
  uint8_t shown = (flashBoost > matrixBase) ? flashBoost : matrixBase;
  matrix.setBrightness(shown);
  matrix.fillScreen(matrix.Color(255, 255, 255));
  matrix.show();

  // ---- PWM LIGHT ----
  // Pulses with the random wave; a MIDI flash overrides upward when brighter.
  uint8_t lightDuty = (flashBoost > pulseVal) ? flashBoost : pulseVal;
  lightWriteDuty(lightDuty);
}
