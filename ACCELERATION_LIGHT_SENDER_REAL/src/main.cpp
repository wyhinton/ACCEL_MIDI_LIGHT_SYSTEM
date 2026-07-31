/*
  Soft Random – SENDER (ESP32-S3 RGB LED Matrix, Waveshare)

  Standalone board: no ESP-NOW, no other ESP32s. The 8×8 RGB matrix and the
  PWM light on GPIO 2 pulse smoothly and randomly (brightness eases between
  random targets over random durations), scaled by a live audio level
  streamed directly from a PC over BLE (see AUDIO_BRIDGE.md).
*/

#include <Arduino.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// -------- AUDIO LEVEL BLE BRIDGE (PC -> here, direct) --------
// A host (e.g. a Mac/PC app, see host/audio_bridge.py) connects over BLE and
// writes a single byte — the smoothed level of its outgoing audio (0..255) —
// to the characteristic below, at ~30-60 Hz. If none arrive for a while we
// fall back to full brightness so the lights never go dark with no PC
// connected.
#define AUDIO_SERVICE_UUID "9a0b0000-1234-4c6e-9b00-1f2e3d4c5b6a"
#define AUDIO_CHAR_UUID    "9a0b0001-1234-4c6e-9b00-1f2e3d4c5b6a"

const unsigned long LEVEL_TIMEOUT_MS      = 1500;  // no level this long => full bright
const unsigned long RED_BLINK_INTERVAL_MS = 3000;  // how often to warn when disconnected

volatile uint8_t       audioLevelRaw = 255;   // last level received (set in BLE write cb)
volatile unsigned long lastLevelMs   = 0;      // when it arrived
float                  audioLevelSmoothed = 255.0f;  // on-device EMA (bridges gaps)

volatile bool bleConnected  = false;   // a BLE client (the PC) is connected
bool          bleWasUp      = false;   // debounced, for the connect burst
unsigned long lastRedBlinkMs = 0;

class LevelWriteCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    uint8_t *data = c->getData();
    size_t   len  = c->getValue().length();
    if (data && len >= 1) {
      audioLevelRaw = data[0];
      lastLevelMs   = millis();
    }
  }
};

// Re-arm advertising after a disconnect (otherwise the host can't reconnect
// without a reboot).
class BridgeServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override    { bleConnected = true; }
  void onDisconnect(BLEServer *s) override { bleConnected = false; BLEDevice::startAdvertising(); }
};

void bleBegin() {
  BLEDevice::init("LightAudioBridge");
  BLEServer  *server = BLEDevice::createServer();
  server->setCallbacks(new BridgeServerCallbacks());
  BLEService *svc    = server->createService(AUDIO_SERVICE_UUID);

  // Write-without-response so the host can stream at audio rate without
  // waiting for an ACK per packet.
  BLECharacteristic *ch = svc->createCharacteristic(
      AUDIO_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  ch->setCallbacks(new LevelWriteCallback());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(AUDIO_SERVICE_UUID);
  adv->setScanResponse(false);
  BLEDevice::startAdvertising();

  Serial.println("BLE audio bridge advertising as 'LightAudioBridge'");
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

// Show BLE link status on the matrix: a green burst the instant the PC
// connects, occasional red while no PC is connected.
void updateBleStatus() {
  unsigned long now = millis();

  if (bleConnected && !bleWasUp) {
    bleWasUp = true;
    Serial.println("BLE LINK UP (PC connected)");
    blinkMatrix(0, 255, 0, 4, 150, 120);   // green: PC connected
  } else if (!bleConnected) {
    if (bleWasUp) {
      bleWasUp = false;
      Serial.println("BLE LINK DOWN (PC disconnected)");
    }
    if (now - lastRedBlinkMs >= RED_BLINK_INTERVAL_MS) {
      lastRedBlinkMs = now;
      blinkMatrix(255, 0, 0, 1, 150, 0);   // red: no PC yet
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // True-random seed so multiple boards don't pulse in lockstep.
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

  bleBegin();   // BLE endpoint for the PC's streamed audio level

  Serial.println("Soft random pulse running.");
}

void loop() {
  while (Serial.available()) Serial.read();   // drain unused serial input

  updateBleStatus();             // show green/red BLE link status

  unsigned long now = millis();

  // ---- AUDIO-LEVEL MULTIPLIER ----
  // Track the PC's streamed level with a light EMA so dropped packets don't
  // cause flicker; fall back to full brightness if the stream goes silent.
  uint8_t levelTarget = (lastLevelMs != 0 && now - lastLevelMs < LEVEL_TIMEOUT_MS)
                          ? audioLevelRaw : 255;
  audioLevelSmoothed += ((float)levelTarget - audioLevelSmoothed) * 0.2f;
  float audioMul = audioLevelSmoothed / 255.0f;   // 0..1

  // ---- SMOOTH RANDOM PULSE (base layer), scaled by the audio level ----
  uint8_t pulseVal   = (uint8_t)(pulse.value(now) * audioMul);      // 0..255
  uint8_t matrixBase = (uint8_t)((uint16_t)pulseVal * MATRIX_MAX_BRIGHTNESS / 255);

  // ---- RENDER MATRIX (every loop) ----
  matrix.setBrightness(matrixBase);
  matrix.fillScreen(matrix.Color(255, 255, 255));
  matrix.show();

  // ---- PWM LIGHT ----
  lightWriteDuty(pulseVal);
}
