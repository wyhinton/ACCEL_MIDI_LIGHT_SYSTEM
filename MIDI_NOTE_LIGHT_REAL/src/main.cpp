/*
  MIDI Note Flash – ESP32-S3 / Seeed XIAO ESP32-S3

  Watches a serial MIDI input for Note On messages. Every time a Note On
  arrives (with non-zero velocity), the whole 8x8 RGB LED matrix flashes
  white for a short period, then turns back off.

  Hardware:
    - Seeed XIAO ESP32-S3
    - 8x8 WS2812 / NeoPixel matrix on MATRIX_PIN
    - Relay module on RELAY_PIN (mirrors the receiver board): relay IN -> RELAY_PIN,
      relay COM/NO -> the light + its supply. Pulsed on for the duration of each flash.
    - MIDI input (e.g. MIDI FeatherWing) wired to the UART used by Serial1

  MIDI plumbing follows the Adafruit MIDI FeatherWing note player example.
*/

#include <Arduino.h>
#include <MIDI.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#include <WiFi.h>
#include <esp_now.h>

// -------- MIDI INPUT --------
// The MIDI FeatherWing uses the hardware UART at 31250 baud. We put MIDI on
// Serial1 so it stays off the USB Serial used for programming/debug. The XIAO
// ESP32-S3 has native USB, so the UART pins are free (this is the "native USB
// works fine" case in the Adafruit MIDI FeatherWing docs).
//
// These are the XIAO ESP32-S3's silkscreened UART pins:
//   D7 = GPIO44 = RX  (wire to the wing's TX / MIDI IN)
//   D6 = GPIO43 = TX  (wire to the wing's RX; unused for input-only)
#define MIDI_RX_PIN 44
#define MIDI_TX_PIN 43

HardwareSerial MidiSerial(1);
MIDI_CREATE_INSTANCE(HardwareSerial, MidiSerial, MIDI);

// -------- RAW DEBUG MODE --------
// Set to 1 to BYPASS the MIDI parser and print every raw byte arriving on the
// UART. Use this first to confirm anything is reaching the board at all — the
// MIDI library silently drops bytes it doesn't recognize as valid MIDI, so if
// wiring/baud is wrong you'd see nothing in normal mode. Set back to 0 once
// you've confirmed bytes are flowing.
#define RAW_MIDI_DUMP 0

// -------- LED MATRIX --------
// Matches the sender/receiver boards: 8x8, data on GPIO 14.
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

// -------- RELAY / LIGHT --------
// A relay is driven in lockstep with the matrix flash, mirroring the receiver
// board: the relay closes on a Note On (channel 1) and reopens when the flash
// duration elapses. Relay is on/off only, so velocity scales the matrix
// brightness/duration but not the relay (it just follows the flash window).
#define RELAY_PIN         2     // GPIO driving the relay IN pin (D1 on the XIAO)
#define RELAY_ACTIVE_HIGH true  // true: HIGH = relay ON. Set false for active-low modules.

void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

// -------- FLASH SCALING (by velocity) --------
// MIDI velocity (1..127) is mapped onto two output ranges — brightness and
// duration — each shaped by a curve exponent:
//   curve = 1.0  -> linear
//   curve > 1.0  -> ease-in  (soft notes stay dim/short, only hard hits pop)
//   curve < 1.0  -> ease-out (even soft notes are fairly bright/long)
// Tune the min/max and curve to taste.
#define VEL_MIN              1     // input velocity range (MIDI is 1..127)
#define VEL_MAX            127

#define FLASH_BRIGHT_MIN    150     // matrix brightness at min velocity (0..255)
#define FLASH_BRIGHT_MAX   255     // matrix brightness at max velocity
#define FLASH_BRIGHT_CURVE  2.0f   // brightness response curve

#define FLASH_DUR_MIN_MS    40     // flash length at min velocity (ms)
#define FLASH_DUR_MAX_MS   80     // flash length at max velocity (ms)
#define FLASH_DUR_CURVE     1.5f   // duration response curve

// -------- FLASH STATE --------
bool          flashActive    = false;
unsigned long flashStartMs   = 0;
unsigned long flashDurationMs = 0;   // set per Note On from velocity

// Map an input through a range with a curve (gamma) shaping. Input is clamped
// to [inMin, inMax]; output is in [outMin, outMax].
float mapCurved(float in, float inMin, float inMax,
                float outMin, float outMax, float curve) {
  float t = (in - inMin) / (inMax - inMin);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  t = powf(t, curve);
  return outMin + t * (outMax - outMin);
}

// -------- POWER INDICATOR --------
// A single dim green pixel in the bottom-right corner shows the board is
// powered/idle. It's the resting state between flashes.
#define POWER_LED_BRIGHTNESS 40   // dim so it isn't distracting (0..255)

void showPowerIndicator() {
  matrix.setBrightness(POWER_LED_BRIGHTNESS);
  matrix.fillScreen(0);
  // Bottom-right corner pixel.
  matrix.drawPixel(MATRIX_WIDTH - 1, MATRIX_HEIGHT - 1, matrix.Color(0, 255, 0));
  matrix.show();
}

void flashOn(uint8_t brightness) {
  relayWrite(true);   // close the relay for the duration of the flash
  matrix.setBrightness(brightness);
  matrix.fillScreen(matrix.Color(255, 255, 255));
  matrix.show();
}

// Resting state after a flash: just the green power dot, not full black.
void flashOff() {
  relayWrite(false);   // reopen the relay
  showPowerIndicator();
}

// -------- ESP-NOW (mirror the flash to the receiver board) --------
// Broadcast a velocity-scaled flash command so ACCELERATION_LIGHT_RECIEVER
// flashes its matrix + relay light with the same brightness/duration. Default
// target is the broadcast address (any receiver on the same WiFi channel).
uint8_t espNowPeerMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
bool    espNowReady      = false;

// Must match the receiver's struct byte-for-byte (5 bytes packed).
typedef struct __attribute__((packed)) {
  uint8_t  cmd;         // = FLASH_CMD_MAGIC
  uint8_t  velocity;    // original MIDI velocity 1..127
  uint8_t  brightness;  // matrix brightness 0..255 (already scaled)
  uint16_t durationMs;  // flash duration in ms (already scaled)
} FlashCommand;
#define FLASH_CMD_MAGIC 0xF1

// Tiny handshake beacon so the receiver's link indicator stays green (it
// red-blinks the matrix when it hasn't heard a HELLO recently). 1 byte, must
// match the receiver's HandshakeMessage.
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2 };
typedef struct __attribute__((packed)) { uint8_t type; } HandshakeMessage;

const unsigned long HELLO_INTERVAL_MS = 1000;
unsigned long lastHelloSentMs = 0;

void espNowBegin() {
  // ESP-NOW rides on the WiFi radio; station mode, not joined to any AP.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("This board STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, espNowPeerMac, 6);
  peer.channel = 0;       // use the current WiFi channel
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ESP-NOW add peer FAILED");
    return;
  }

  espNowReady = true;
  Serial.println("ESP-NOW ready (flash commands enabled)");
}

void espNowSendFlash(uint8_t velocity, uint8_t brightness, uint16_t durationMs) {
  if (!espNowReady) return;
  FlashCommand fc;
  fc.cmd        = FLASH_CMD_MAGIC;
  fc.velocity   = velocity;
  fc.brightness = brightness;
  fc.durationMs = durationMs;
  esp_now_send(espNowPeerMac, (const uint8_t *)&fc, sizeof(fc));
}

void espNowSendHello() {
  if (!espNowReady) return;
  HandshakeMessage hs;
  hs.type = HS_HELLO;
  esp_now_send(espNowPeerMac, (const uint8_t *)&hs, sizeof(hs));
}

// Running total of Note On messages we've flashed for, for debug context.
unsigned long noteOnCount = 0;

// Called by the MIDI library for every Note On message.
void handleNoteOn(byte channel, byte note, byte velocity) {
  // Only flash for channel 1. MIDI.begin(1) already filters to channel 1, but
  // guard explicitly so the relay never fires on other channels.
  if (channel != 1) return;

  // A Note On with velocity 0 is the conventional "note off" — ignore it.
  if (velocity == 0) {
    Serial.print("[");      Serial.print(millis());
    Serial.print(" ms] Note On (vel=0 => note off) ch="); Serial.print(channel);
    Serial.print(" note="); Serial.println(note);
    return;
  }

  // Scale brightness and duration from velocity, each with its own curve.
  uint8_t       brightness = (uint8_t)mapCurved(velocity, VEL_MIN, VEL_MAX,
                               FLASH_BRIGHT_MIN, FLASH_BRIGHT_MAX, FLASH_BRIGHT_CURVE);
  unsigned long duration   = (unsigned long)mapCurved(velocity, VEL_MIN, VEL_MAX,
                               FLASH_DUR_MIN_MS, FLASH_DUR_MAX_MS, FLASH_DUR_CURVE);

  flashOn(brightness);
  flashActive     = true;
  flashStartMs    = millis();
  flashDurationMs = duration;
  noteOnCount++;

  // Mirror the same scaled flash to the receiver board over ESP-NOW.
  espNowSendFlash(velocity, brightness, (uint16_t)duration);

  Serial.print("[");        Serial.print(flashStartMs);
  Serial.print(" ms] Note On  ch=");  Serial.print(channel);
  Serial.print(" note=");   Serial.print(note);
  Serial.print(" vel=");    Serial.print(velocity);
  Serial.print("  -> FLASH ON  bright="); Serial.print(brightness);
  Serial.print(" dur=");    Serial.print(duration);
  Serial.print("ms (#");    Serial.print(noteOnCount);
  Serial.println(")");
}

// Called for every Note Off message — handy to see the full note lifecycle.
void handleNoteOff(byte channel, byte note, byte velocity) {
  Serial.print("[");        Serial.print(millis());
  Serial.print(" ms] Note Off ch="); Serial.print(channel);
  Serial.print(" note=");   Serial.print(note);
  Serial.print(" vel=");    Serial.println(velocity);
}

// Catch-all for anything that isn't a Note On/Off, so unexpected traffic is
// visible instead of silently dropped.
void handleOtherMessage(const midi::Message<128> &msg) {
  if (msg.type == midi::NoteOn || msg.type == midi::NoteOff) return;
  // Don't spam the log with the continuous clock / active-sensing stream.
  if (msg.type == midi::Clock || msg.type == midi::ActiveSensing) return;
  Serial.print("[");        Serial.print(millis());
  Serial.print(" ms] MIDI msg type=0x"); Serial.print(msg.type, HEX);
  Serial.print(" ch=");     Serial.print(msg.channel);
  Serial.print(" d1=");     Serial.print(msg.data1);
  Serial.print(" d2=");     Serial.println(msg.data2);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);      // light off at boot

  matrix.begin();
  showPowerIndicator();   // green corner dot = powered/idle

  // Standard MIDI baud is 31250. Map Serial1 onto the chosen pins.
  MidiSerial.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);

  espNowBegin();   // WiFi/ESP-NOW radio for mirroring flashes to the receiver

#if RAW_MIDI_DUMP
  Serial.println("=================================================");
  Serial.println("RAW MIDI DUMP MODE - printing every byte on the UART.");
  Serial.print  ("  Listening on Serial1 RX=GPIO"); Serial.print(MIDI_RX_PIN);
  Serial.println(" @ 31250 baud, 8N1");
  Serial.println("  If you see NOTHING here, no bytes are reaching the pin:");
  Serial.println("    - check wiring TX(source) -> RX=GPIO44, common GND");
  Serial.println("    - confirm the source is actually sending");
  Serial.println("    - try swapping RX/TX, or a different baud");
  Serial.println("=================================================");
  return;   // skip MIDI library setup; loop() handles the raw dump
#endif

  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleMessage(handleOtherMessage);   // everything else
  MIDI.begin(1);   // listen only on MIDI channel 1

  Serial.println("=================================================");
  Serial.println("MIDI note flash ready - waiting for Note On.");
  Serial.print  ("  MIDI on Serial1: RX=GPIO"); Serial.print(MIDI_RX_PIN);
  Serial.print  (" TX=GPIO");                   Serial.print(MIDI_TX_PIN);
  Serial.println(" @ 31250 baud");
  Serial.print  ("  Matrix: ");  Serial.print(MATRIX_WIDTH);
  Serial.print  ("x");           Serial.print(MATRIX_HEIGHT);
  Serial.print  (" on GPIO");    Serial.print(MATRIX_PIN);
  Serial.println();
  Serial.print  ("  Flash by velocity: bright ");
  Serial.print(FLASH_BRIGHT_MIN); Serial.print(".."); Serial.print(FLASH_BRIGHT_MAX);
  Serial.print  (" (curve "); Serial.print(FLASH_BRIGHT_CURVE); Serial.print(")");
  Serial.print  (", dur ");
  Serial.print(FLASH_DUR_MIN_MS); Serial.print(".."); Serial.print(FLASH_DUR_MAX_MS);
  Serial.print  ("ms (curve "); Serial.print(FLASH_DUR_CURVE); Serial.println(")");
  Serial.println("=================================================");
}

// Heartbeat: prove the board is alive even when no MIDI is arriving.
#define HEARTBEAT_INTERVAL_MS 5000
unsigned long lastHeartbeatMs = 0;

void loop() {
#if RAW_MIDI_DUMP
  // Dump every raw byte as hex, 16 per line with the timestamp of the first
  // byte on each line. Flash the matrix briefly on ANY byte so you also get a
  // visual confirmation without watching the serial monitor.
  static int  bytesOnLine = 0;
  while (MidiSerial.available()) {
    uint8_t b = MidiSerial.read();

    // Skip System Real-Time spam so real messages are visible:
    //   0xF8 Timing Clock, 0xFE Active Sensing. (Keep Start/Stop/Continue
    //   0xFA/0xFB/0xFC and everything else.)
    if (b == 0xF8 || b == 0xFE) continue;

    if (bytesOnLine == 0) {
      Serial.print("[");  Serial.print(millis());  Serial.print(" ms] RX:");
    }
    Serial.print(" ");
    if (b < 0x10) Serial.print("0");   // pad to two hex digits
    Serial.print(b, HEX);

    if (++bytesOnLine >= 16) { Serial.println(); bytesOnLine = 0; }

    // Flash only on a Note On status byte (0x90-0x9F) so the matrix isn't
    // pinned on by continuous traffic. (Raw mode can't see velocity here, so
    // it uses a fixed full-brightness blip.)
    if ((b & 0xF0) == 0x90) {
      flashOn(FLASH_BRIGHT_MAX);
      flashActive     = true;
      flashStartMs    = millis();
      flashDurationMs = FLASH_DUR_MIN_MS;
    }
  }

  // Turn the flash off after the duration (same non-blocking logic as below).
  if (flashActive && (millis() - flashStartMs >= flashDurationMs)) {
    flashActive = false;
    flashOff();
  }

  // Heartbeat so an idle line is obvious vs. a hung board.
  unsigned long nowRaw = millis();
  if (nowRaw - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = nowRaw;
    if (bytesOnLine > 0) { Serial.println(); bytesOnLine = 0; }  // flush partial line
    Serial.print("["); Serial.print(nowRaw);
    Serial.println(" ms] (raw dump) no bytes / idle");
  }
  return;
#endif

  // Pump the MIDI parser; fires the handlers on incoming messages.
  MIDI.read();

  // Beacon HELLO so the receiver's link indicator stays green between notes.
  if (millis() - lastHelloSentMs >= HELLO_INTERVAL_MS) {
    lastHelloSentMs = millis();
    espNowSendHello();
  }

  // Non-blocking: turn the matrix back off once the flash duration elapses.
  if (flashActive && (millis() - flashStartMs >= flashDurationMs)) {
    flashActive = false;
    flashOff();
    Serial.print("["); Serial.print(millis());
    Serial.println(" ms] FLASH OFF");
  }

  // Periodic alive ping with a running count of notes seen so far.
  unsigned long now = millis();
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    Serial.print("["); Serial.print(now);
    Serial.print(" ms] alive - noteOns="); Serial.print(noteOnCount);
    Serial.print(" flashActive=");          Serial.println(flashActive ? "yes" : "no");
  }
}
