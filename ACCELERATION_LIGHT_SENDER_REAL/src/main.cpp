/* 
  Project 4: Tilt Dot – ESP32-S3 RGB LED Matrix (Waveshare)

  Reads tilt from the QMI8658C IMU and moves a dot on the 8×8 RGB LED matrix.
  If acceleration exceeds a threshold, the entire matrix flashes white.
*/

#include <Arduino.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#include <QMI8658.h>   // by Lahav Gahali

#include <BLEMidi.h>

#include <WiFi.h>
#include <esp_now.h>

void OnConnected()    { Serial.println("BLE MIDI connected"); }
void OnDisconnected() { Serial.println("BLE MIDI disconnected"); }

// -------- ESP-NOW (impact message to another ESP32) --------
// Default target is the broadcast address: ANY ESP32 running an ESP-NOW
// receiver on the same WiFi channel will get the message — no need to know
// its MAC. To target ONE specific board instead, replace this with that
// board's STA MAC (print WiFi.macAddress() on the receiver to find it),
// e.g. { 0x24, 0x62, 0xAB, 0xCD, 0xEF, 0x01 }.
uint8_t espNowPeerMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Payload sent on every impact. Keep small (ESP-NOW max is 250 bytes).
typedef struct __attribute__((packed)) {
  uint32_t seq;       // increments each impact
  float    jerk;      // measured jerk (g/frame) that triggered it
  float    accelMag;  // acceleration magnitude at impact (g)
} ImpactMessage;

uint32_t espNowImpactSeq = 0;
bool     espNowReady     = false;

// -------- HANDSHAKE / LINK STATUS --------
// A tiny separate message confirms the two boards can hear each other. The
// sender beacons HELLO; the receiver replies ACK. Distinguished from
// ImpactMessage by length (1 byte vs sizeof(ImpactMessage)).
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2 };
typedef struct __attribute__((packed)) {
  uint8_t type;
} HandshakeMessage;

// -------- FLASH COMMAND (from the MIDI board) --------
// Velocity-scaled flash request broadcast by MIDI_NOTE_LIGHT_REAL. We flash
// the matrix (at the given brightness) and pulse the light for the duration.
// Distinguished by length: Handshake=1, FlashCommand=5, Impact=12 bytes.
typedef struct __attribute__((packed)) {
  uint8_t  cmd;         // = FLASH_CMD_MAGIC
  uint8_t  velocity;    // original MIDI velocity 1..127 (for logging)
  uint8_t  brightness;  // matrix brightness 0..255 (already scaled)
  uint16_t durationMs;  // flash duration in ms (already scaled)
} FlashCommand;
#define FLASH_CMD_MAGIC 0xF1

// MIDI-triggered flash state (set in the recv callback, rendered in loop()).
volatile bool          midiFlashActive    = false;
volatile bool          newMidiFlash       = false;
volatile unsigned long midiFlashStartMs   = 0;
volatile unsigned long midiFlashDurationMs = 0;
volatile uint8_t       midiFlashBrightness = 255;
volatile uint8_t       midiFlashVelocity  = 0;

// Latches true on the FIRST MIDI flash command. Once set, the PWM light stops
// its always-on idle/crash-fade behavior and is driven only by MIDI strobes
// (off when idle, full during a flash) — matching the receiver board.
volatile bool          midiModeActive     = false;

const unsigned long HELLO_INTERVAL_MS     = 1000;  // beacon period
const unsigned long LINK_TIMEOUT_MS       = 3000;  // no ACK this long => link down
const unsigned long RED_BLINK_INTERVAL_MS = 3000;  // how often to warn when down

volatile unsigned long lastAckMs = 0;   // last ACK arrival (set in recv cb)
bool          linkUp          = false;  // debounced link state
unsigned long lastHelloSentMs = 0;
unsigned long lastRedBlinkMs  = 0;

// Send-status callback (optional, for debugging delivery).
// Signature differs across Arduino-ESP32 core versions.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  Serial.print("ESP-NOW send: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// Receive callback: listen for the receiver's ACK to confirm the link is up.
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
    midiModeActive      = true;   // latch: disable always-on idle light
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
  Serial.println("ESP-NOW ready (impact messages enabled)");
}

void espNowSendImpact(float jerk, float accelMag) {
  if (!espNowReady) return;

  ImpactMessage msg;
  msg.seq      = ++espNowImpactSeq;
  msg.jerk     = jerk;
  msg.accelMag = accelMag;

  esp_err_t r = esp_now_send(espNowPeerMac, (const uint8_t *)&msg, sizeof(msg));
  if (r != ESP_OK) {
    Serial.print("ESP-NOW send queue error: ");
    Serial.println(r);
  }
}

void espNowSendHello() {
  if (!espNowReady) return;
  HandshakeMessage hs;
  hs.type = HS_HELLO;
  esp_now_send(espNowPeerMac, (const uint8_t *)&hs, sizeof(hs));
}

// -------- CRASH LIGHT (PWM on GPIO 2) --------
// Idle: half brightness. On crash: snap to full, then fade back to half.
#define LIGHT_PIN           2      // PWM-capable GPIO driving the light
#define LIGHT_PWM_FREQ      5000   // Hz
#define LIGHT_PWM_RES_BITS  8      // 8-bit duty -> 0..255
#define LIGHT_PWM_CHANNEL   0      // LEDC channel (core 2.x only)

// Runtime-tunable over serial (params 4/5/6)
int   lightHalfDuty       = 175;     // ~half brightness when idle (0-255)
int   lightFullDuty       = 255;     // full brightness at moment of crash (0-255)
float lightFadeDurationMs = 1500.0f; // full -> half fade time (ms)

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
  lightWriteDuty(lightHalfDuty);   // start at half
}

// -------- LED MATRIX SETUP --------
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

// -------- QMI8658 IMU SETUP --------
QMI8658 imu;
QMI8658_Data imuData;

// -------- USER SETTINGS --------

// true  -> dot on opposite side
// false -> dot on same side as UP
bool useOppositeMapping = false;

// Dot color
uint8_t dotRed   = 0;
uint8_t dotGreen = 100;
uint8_t dotBlue  = 0;

// Acceleration flash threshold (g)
// (kept for reference – collision now uses jerk instead)
// const float accelFlashThreshold = 2.6f;

// -------- COLLISION DETECTION SETTINGS (runtime-tunable) --------
// Adjust over serial: 1/2/3 selects param, arrow keys ±0.01, shift+arrow ±0.1
// (cooldown steps are ×1000: arrow=10ms, shift+arrow=100ms)
float collisionJerkThreshold = 1.0f;   // g/frame – lower = more sensitive
float minMovingMag           = 1.3f;   // previous frame must exceed this (car was moving)
float collisionCooldownMs    = 600.0f; // ms before re-triggering

// Board sides
enum Side {
  SIDE_CENTER = 0,
  SIDE_USB,
  SIDE_OUSB,
  SIDE_15,
  SIDE_34
};

// Smooth dot position
float dotPosX = 3.0f;
float dotPosY = 3.0f;

const float dotSmooth = 0.25f;

bool isFlat = false;

// -------- COLLISION STATE --------
float prevAccelMag          = 1.0f;
unsigned long lastCollisionMs = 0;

// Drive the GPIO 2 light: full at the instant of a crash, linear fade
// back to half over lightFadeDurationMs. Keys off lastCollisionMs.
void updateCrashLight() {
  // After the first MIDI flash, the light is MIDI-strobed only (off at idle).
  // The MIDI flash block in loop() owns the light from here on.
  if (midiModeActive) return;

  unsigned long sinceCrash = millis() - lastCollisionMs;
  uint8_t duty;
  if (lastCollisionMs == 0 || sinceCrash >= lightFadeDurationMs) {
    duty = lightHalfDuty;
  } else {
    float f = 1.0f - (float)sinceCrash / (float)lightFadeDurationMs; // 1 -> 0
    duty = lightHalfDuty + (uint8_t)((lightFullDuty - lightHalfDuty) * f);
  }
  lightWriteDuty(duty);
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

// -------- SERIAL TUNING STATE --------
// 1=jerkThreshold 2=minMovingMag 3=cooldownMs 4=lightHalf 5=lightFull 6=lightFadeMs
int selectedParam = 1;

void printParams() {
  Serial.println("\n--- Tunable params ---");
  Serial.print(selectedParam == 1 ? "[1]" : " 1 ");
  Serial.print(" jerkThreshold  = "); Serial.println(collisionJerkThreshold, 3);
  Serial.print(selectedParam == 2 ? "[2]" : " 2 ");
  Serial.print(" minMovingMag   = "); Serial.println(minMovingMag, 3);
  Serial.print(selectedParam == 3 ? "[3]" : " 3 ");
  Serial.print(" cooldownMs     = "); Serial.println((int)collisionCooldownMs);
  Serial.print(selectedParam == 4 ? "[4]" : " 4 ");
  Serial.print(" lightHalfDuty  = "); Serial.println(lightHalfDuty);
  Serial.print(selectedParam == 5 ? "[5]" : " 5 ");
  Serial.print(" lightFullDuty  = "); Serial.println(lightFullDuty);
  Serial.print(selectedParam == 6 ? "[6]" : " 6 ");
  Serial.print(" lightFadeMs    = "); Serial.println((int)lightFadeDurationMs);
  Serial.println("----------------------");
}

// Adjust the selected param. sign = +1 (up) / -1 (down). shift = larger step.
void adjustParam(int sign, bool shift) {
  switch (selectedParam) {
    case 1: collisionJerkThreshold += sign * (shift ? 0.1f : 0.01f);
            if (collisionJerkThreshold < 0.01f) collisionJerkThreshold = 0.01f; break;
    case 2: minMovingMag += sign * (shift ? 0.1f : 0.01f);
            if (minMovingMag < 0.0f) minMovingMag = 0.0f; break;
    case 3: collisionCooldownMs += sign * (shift ? 100.0f : 10.0f);
            if (collisionCooldownMs < 0.0f) collisionCooldownMs = 0.0f; break;
    case 4: lightHalfDuty += sign * (shift ? 10 : 1);
            lightHalfDuty = constrain(lightHalfDuty, 0, 255); break;
    case 5: lightFullDuty += sign * (shift ? 10 : 1);
            lightFullDuty = constrain(lightFullDuty, 0, 255); break;
    case 6: lightFadeDurationMs += sign * (shift ? 100.0f : 10.0f);
            if (lightFadeDurationMs < 0.0f) lightFadeDurationMs = 0.0f; break;
  }
  printParams();
}

// Parser states for ANSI escape sequences
enum EscState { ES_IDLE, ES_ESC, ES_BRACKET, ES_P1, ES_SEMI, ES_P2 };
EscState escState = ES_IDLE;
char escP1 = 0, escP2 = 0;

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();

    switch (escState) {

      case ES_IDLE:
        if (c == 0x1b)       { escState = ES_ESC; }
        else if (c >= '1' && c <= '6') {
          selectedParam = c - '0';
          Serial.print("Selected param "); Serial.println(selectedParam);
          printParams();
        }
        break;

      case ES_ESC:
        escState = (c == '[') ? ES_BRACKET : ES_IDLE;
        break;

      case ES_BRACKET:
        if (c == '1')        { escP1 = '1'; escState = ES_P1; }
        else if (c == 'A' || c == 'B') { // plain arrow: A=up, B=down
          adjustParam((c == 'A') ? 1 : -1, false);
          escState = ES_IDLE;
        }
        else                 { escState = ES_IDLE; }
        break;

      case ES_P1:  // consumed '1'
        escState = (c == ';') ? ES_SEMI : ES_IDLE;
        break;

      case ES_SEMI: // consumed ';'
        escP2 = c;
        escState = ES_P2;
        break;

      case ES_P2:  // consumed modifier digit, next byte is the letter
        if (escP2 == '2' && (c == 'A' || c == 'B')) { // shift+arrow
          adjustParam((c == 'A') ? 1 : -1, true);
        }
        escState = ES_IDLE;
        break;
    }
  }
}

// -------- BLE MIDI STATE --------
bool midiNotePlaying = false;
const uint8_t MIDI_NOTE_C1  = 36;   // C1 in standard MIDI (C4=60)
const uint8_t MIDI_VELOCITY = 127;
const uint8_t MIDI_CHANNEL  = 1;

const char* sideName(Side s) {
  switch (s) {
    case SIDE_CENTER: return "CENTER";
    case SIDE_USB:    return "USB";
    case SIDE_OUSB:   return "OUSB";
    case SIDE_15:     return "15";
    case SIDE_34:     return "34";
    default:          return "?";
  }
}

// Detect which side is UP
Side detectSideUp(float ax_g, float ay_g, float az_g) {

  const float flatThreshXY = 0.15f;
  const float flatThreshZ  = 0.15f;

  if (fabs(ax_g) < flatThreshXY &&
      fabs(ay_g) < flatThreshXY &&
      fabs(az_g - 1.0f) < flatThreshZ) {
    isFlat = true;
    return SIDE_CENTER;
  }

  isFlat = false;

  const float tiltThreshY = 0.5f;
  const float tiltThreshX = 0.5f;

  if (fabs(ay_g) >= tiltThreshY) {
    if (ay_g > 0) {
      return SIDE_15;
    } else {
      return SIDE_34;
    }
  }

  if (fabs(ax_g) >= tiltThreshX) {
    if (ax_g > 0) {
      return SIDE_USB;
    } else {
      return SIDE_OUSB;
    }
  }

  return SIDE_CENTER;
}

// Map from UP side to dot location
Side dotSideFromUpSide(Side upSide) {

  switch (upSide) {

    case SIDE_USB:
      return useOppositeMapping ? SIDE_OUSB : SIDE_USB;

    case SIDE_OUSB:
      return useOppositeMapping ? SIDE_USB : SIDE_OUSB;

    case SIDE_34:
      return useOppositeMapping ? SIDE_15 : SIDE_34;

    case SIDE_15:
      return useOppositeMapping ? SIDE_34 : SIDE_15;

    default:
      return SIDE_CENTER;
  }
}

// Convert side to pixel location
void getDotPixel(Side dotSide, int &px, int &py) {

  switch (dotSide) {
    case SIDE_USB:   px = 3; py = 0; break;
    case SIDE_OUSB:  px = 3; py = 7; break;
    case SIDE_15:    px = 0; py = 3; break;
    case SIDE_34:    px = 7; py = 3; break;
    default:         px = 3; py = 3; break;
  }
}

void setup() {

  Serial.begin(115200);
  delay(500);

  lightBegin();   // PWM light at half brightness

  matrix.begin();
  matrix.setBrightness(20);
  matrix.fillScreen(0);
  matrix.show();

  // Startup banner: 'S' identifies this board as the SENDER.
  showStartupLetter('S', 0, 0, 255, 1000);

  // IMU: SDA=11, SCL=12
  if (!imu.begin(11, 12)) {
    Serial.println("Failed to initialize QMI8658!");
    while (1) { delay(1000); }
  }

  imu.setAccelUnit_mg(true);
  imu.setGyroUnit_dps(true);
  imu.setDisplayPrecision(4);

  Serial.println("QMI8658 initialized.");

  espNowBegin();   // WiFi/ESP-NOW radio for impact messages

  BLEMidiServer.begin("AccelLight");
  BLEMidiServer.setOnConnectCallback(OnConnected);
  BLEMidiServer.setOnDisconnectCallback(OnDisconnected);
  Serial.println("BLE MIDI started – device: AccelLight");
  printParams();
}

void loop() {

  updateCrashLight();   // runs every loop so the fade continues through early returns

  handleSerial();

  updateLinkStatus();   // beacon HELLO + show green/red link status

  unsigned long now = millis();

  // ---- MIDI FLASH ENVELOPE (from the MIDI board over ESP-NOW) ----
  // Recomputed every loop from midiFlashStartMs, so a new note retriggers it
  // seamlessly even mid-decay — nothing is missed in quick succession. The
  // envelope decays linearly from the velocity-scaled peak back to 0, and is
  // layered ON TOP of the matrix pulse (not a blocking takeover).
  uint8_t flashBoost = 0;
  if (midiModeActive) {
    unsigned long since = now - midiFlashStartMs;
    if (since < midiFlashDurationMs) {
      float env = 1.0f - (float)since / (float)midiFlashDurationMs; // 1 -> 0
      flashBoost = (uint8_t)(env * midiFlashBrightness);
    }
  }
  if (newMidiFlash) {
    newMidiFlash = false;
    Serial.print("MIDI FLASH  vel=");  Serial.print(midiFlashVelocity);
    Serial.print(" bright=");          Serial.print(midiFlashBrightness);
    Serial.print(" dur=");             Serial.print(midiFlashDurationMs);
    Serial.println("ms");
  }

  // ---- IMU SAMPLING + COLLISION (throttled, non-blocking) ----
  // Sampled ~every 80ms via millis() instead of delay(), so the matrix/light
  // flash below renders every loop and keeps up with rapid notes.
  const unsigned long SAMPLE_INTERVAL_MS = 80;
  static unsigned long lastSampleMs = 0;
  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS && imu.readSensorData(imuData)) {
    lastSampleMs = now;

    float ax_g = imuData.accelX / 1000.0f;
    float ay_g = imuData.accelY / 1000.0f;
    float az_g = imuData.accelZ / 1000.0f;
    float accelMag = sqrt(ax_g * ax_g + ay_g * ay_g + az_g * az_g);

    float jerk = fabs(accelMag - prevAccelMag);
    bool collision = (jerk > collisionJerkThreshold)
                  && (prevAccelMag > minMovingMag)
                  && ((now - lastCollisionMs) > (unsigned long)collisionCooldownMs);
    prevAccelMag = accelMag;

    if (collision) {
      lastCollisionMs = now;   // render below shows the crash flash window
      if (!midiNotePlaying && BLEMidiServer.isConnected()) {
        BLEMidiServer.noteOn(MIDI_CHANNEL - 1, MIDI_NOTE_C1, MIDI_VELOCITY);
        midiNotePlaying = true;
        Serial.println("MIDI NoteOn  C1");
      }
      espNowSendImpact(jerk, accelMag);   // notify the other ESP32
      Serial.print("COLLISION DETECTED  jerk="); Serial.println(jerk, 3);
    } else if (midiNotePlaying) {
      // Threshold no longer exceeded – release note
      BLEMidiServer.noteOff(MIDI_CHANNEL - 1, MIDI_NOTE_C1, 0);
      midiNotePlaying = false;
      Serial.println("MIDI NoteOff C1");
    }
  }

  // ---- RENDER MATRIX (every loop) ----
  // Base layer: a brief full-white crash flash, otherwise the idle white pulse
  // (0 -> ~30% over 2.5s). The MIDI flash is layered on top by taking the
  // brighter of base vs. flashBoost — both are white, so this reads as the
  // flash popping above the pulse and decaying back into it.
  const unsigned long COLLISION_FLASH_MS  = 120;
  const float         pulsePeriodMs       = 2500.0f;
  const uint8_t       pulseMaxBrightness  = 60;

  uint8_t baseBrightness;
  if (lastCollisionMs != 0 && (now - lastCollisionMs) < COLLISION_FLASH_MS) {
    baseBrightness = 255;   // crash flash
  } else {
    float phase = (now % (unsigned long)pulsePeriodMs) / pulsePeriodMs; // 0..1
    float wave  = (1.0f - cosf(2.0f * PI * phase)) * 0.5f;
    baseBrightness = (uint8_t)(wave * pulseMaxBrightness);
  }

  uint8_t shown = (flashBoost > baseBrightness) ? flashBoost : baseBrightness;
  matrix.setBrightness(shown);
  matrix.fillScreen(matrix.Color(255, 255, 255));
  matrix.show();

  // ---- LIGHT ----
  // In MIDI mode the PWM light is off at idle and strobes+fades with the flash
  // envelope (retriggered per note). Before the first MIDI note, the light is
  // owned by updateCrashLight() at the top of loop().
  if (midiModeActive) {
    lightWriteDuty(flashBoost);
  }
}