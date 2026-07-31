/*
  Jerk/Crash-triggered MIDI – SENDER (ESP32-S3 RGB LED Matrix, Waveshare)

  Reads acceleration from a QMI8658 IMU. A sudden jerk (impact/crash) fires a
  MIDI Note On over BLE-MIDI (the standard Bluetooth-SIG GATT profile), so
  any BLE-MIDI-capable host (Windows Bluetooth LE MIDI, a DAW, etc.) sees it
  as a normal MIDI input once paired — no companion app needed to receive
  notes.

  The 8x8 RGB matrix and the PWM light on GPIO 2 glow with a smooth,
  randomly-wandering idle pulse, boosted by a brief flash on every detected
  impact. A single corner pixel shows live BLE connection status; all pixels
  flash green the instant a BLE-MIDI host connects.
*/

#include <Arduino.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#include <QMI8658.h>   // by Lahav Gahali

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// -------- BLE-MIDI (standard Bluetooth-SIG GATT profile) --------
// Hand-rolled with the raw BLE APIs (rather than a MIDI library) so it's a
// single, self-contained BLE identity/server. Packet format per the
// Bluetooth-SIG "MIDI over Bluetooth Low Energy" spec: a header byte, a
// timestamp byte, then the raw MIDI status/data bytes, sent as a
// characteristic notification.
#define MIDI_SERVICE_UUID "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define MIDI_CHAR_UUID    "7772E5DB-3868-4112-A1A9-F2669D106BF3"

const uint8_t MIDI_NOTE     = 36;   // C1
const uint8_t MIDI_VELOCITY = 127;
const uint8_t MIDI_CHANNEL  = 0;    // 0-indexed (MIDI channel 1)

BLECharacteristic *midiChar = nullptr;

volatile bool bleConnected = false;   // a BLE-MIDI host is connected
bool          bleWasUp     = false;   // debounced, for the connect burst

// Re-arm advertising after a disconnect (otherwise a host can't reconnect
// without a reboot).
class BridgeServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override    { bleConnected = true; }
  void onDisconnect(BLEServer *s) override { bleConnected = false; BLEDevice::startAdvertising(); }
};

// Pack and send one MIDI message as a BLE-MIDI notification.
void sendMidiMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  if (midiChar == nullptr) return;
  uint16_t t = (uint16_t)(millis() & 0x1FFF);   // 13-bit BLE-MIDI timestamp
  uint8_t packet[5] = {
    (uint8_t)(0x80 | ((t >> 7) & 0x3F)),        // header byte
    (uint8_t)(0x80 | (t & 0x7F)),               // timestamp byte
    status, data1, data2,
  };
  midiChar->setValue(packet, 5);
  midiChar->notify();
}

void midiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  sendMidiMessage(0x90 | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

void midiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
  sendMidiMessage(0x80 | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

void bleBegin() {
  BLEDevice::init("AccelLight");
  BLEServer  *server = BLEDevice::createServer();
  server->setCallbacks(new BridgeServerCallbacks());
  BLEService *svc    = server->createService(MIDI_SERVICE_UUID);

  midiChar = svc->createCharacteristic(
      MIDI_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  midiChar->addDescriptor(new BLE2902());   // required for the host to enable notifications

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(MIDI_SERVICE_UUID);
  adv->setScanResponse(false);
  BLEDevice::startAdvertising();

  Serial.println("BLE-MIDI advertising as 'AccelLight'");
}

// -------- SMOOTH RANDOM PULSE (idle ambient animation) --------
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

// Corner pixel reserved as a live BLE status indicator (red=disconnected,
// green=connected), drawn on top of the pulse every frame.
#define STATUS_PIXEL_X 0
#define STATUS_PIXEL_Y 0

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

// BLE link status: a full-matrix green burst the instant a host connects;
// otherwise a single corner pixel tracks live status every frame (see
// STATUS_PIXEL_X/Y in the render step of loop()).
void updateBleStatus() {
  if (bleConnected && !bleWasUp) {
    bleWasUp = true;
    Serial.println("BLE LINK UP (host connected)");
    blinkMatrix(0, 255, 0, 4, 150, 120);   // green: host connected
    matrix.setBrightness(MATRIX_MAX_BRIGHTNESS);   // blinkMatrix leaves brightness at 120
  } else if (!bleConnected && bleWasUp) {
    bleWasUp = false;
    Serial.println("BLE LINK DOWN (host disconnected)");
  }
}

// -------- IMU / JERK-CRASH DETECTION --------
QMI8658      imu;
QMI8658_Data imuData;

const unsigned long IMU_SAMPLE_INTERVAL_MS = 80;    // throttle IMU reads
const float          COLLISION_JERK_THRESHOLD = 1.0f;   // g/frame – lower = more sensitive
const float          MIN_MOVING_MAG           = 1.3f;   // previous frame must exceed this
const unsigned long  COLLISION_COOLDOWN_MS    = 600;    // ms before re-triggering
const unsigned long  COLLISION_FLASH_MS       = 120;    // visual flash window

float         prevAccelMag   = 1.0f;
unsigned long lastCollisionMs = 0;
bool          midiNotePlaying = false;

// Sampled ~every IMU_SAMPLE_INTERVAL_MS via millis() instead of delay(), so
// the matrix/light render every loop and keep up with rapid notes.
void updateImuCollision(unsigned long now) {
  static unsigned long lastSampleMs = 0;
  if (now - lastSampleMs < IMU_SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  if (!imu.readSensorData(imuData)) return;

  float ax_g = imuData.accelX / 1000.0f;
  float ay_g = imuData.accelY / 1000.0f;
  float az_g = imuData.accelZ / 1000.0f;
  float accelMag = sqrt(ax_g * ax_g + ay_g * ay_g + az_g * az_g);

  float jerk = fabs(accelMag - prevAccelMag);
  bool collision = (jerk > COLLISION_JERK_THRESHOLD)
                && (prevAccelMag > MIN_MOVING_MAG)
                && ((now - lastCollisionMs) > COLLISION_COOLDOWN_MS);
  prevAccelMag = accelMag;

  if (collision) {
    lastCollisionMs = now;   // render step below shows the crash flash window
    if (!midiNotePlaying && bleConnected) {
      midiNoteOn(MIDI_CHANNEL, MIDI_NOTE, MIDI_VELOCITY);
      midiNotePlaying = true;
      Serial.print("COLLISION DETECTED  jerk="); Serial.println(jerk, 3);
    }
  } else if (midiNotePlaying) {
    midiNoteOff(MIDI_CHANNEL, MIDI_NOTE, 0);
    midiNotePlaying = false;
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

  // Fixed brightness ceiling for normal operation. Held constant (instead of
  // pulsing) so the BLE status pixel stays legible even when the pulse dims.
  matrix.setBrightness(MATRIX_MAX_BRIGHTNESS);

  // Pulse 0..255 intensity, easing over 0.6–2.5 s segments.
  pulse.begin(0, 255, 600, 2500);

  // IMU: SDA=11, SCL=12 (Seeed XIAO ESP32-S3)
  if (!imu.begin(11, 12)) {
    Serial.println("Failed to initialize QMI8658!");
    while (1) { delay(1000); }
  }
  imu.setAccelUnit_mg(true);
  imu.setGyroUnit_dps(true);
  imu.setDisplayPrecision(4);
  Serial.println("QMI8658 initialized.");

  bleBegin();   // BLE-MIDI endpoint, fires a note on jerk/crash

  Serial.println("Jerk/crash-triggered MIDI running.");
}

void loop() {
  while (Serial.available()) Serial.read();   // drain unused serial input

  updateBleStatus();             // show green/red BLE link status

  unsigned long now = millis();

  updateImuCollision(now);       // sample IMU, fire MIDI note on/off

  // ---- SMOOTH RANDOM PULSE (idle ambient layer) ----
  uint8_t pulseVal = pulse.value(now);

  // ---- COLLISION FLASH, layered on top of the pulse ----
  uint8_t flashVal = (lastCollisionMs != 0 && (now - lastCollisionMs) < COLLISION_FLASH_MS)
                       ? 255 : 0;
  uint8_t shown = (flashVal > pulseVal) ? flashVal : pulseVal;

  // ---- RENDER MATRIX (every loop) ----
  // Brightness is held fixed (set once in setup()); the pulse/flash scale the
  // pixel color instead, so the status pixel below stays at full strength.
  matrix.fillScreen(matrix.Color(shown, shown, shown));
  matrix.drawPixel(STATUS_PIXEL_X, STATUS_PIXEL_Y,
                    bleConnected ? matrix.Color(0, 255, 0) : matrix.Color(255, 0, 0));
  matrix.show();

  // ---- PWM LIGHT ----
  lightWriteDuty(shown);
}
