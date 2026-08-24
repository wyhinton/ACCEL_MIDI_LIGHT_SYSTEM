// Pin map (primary board):
//   GPIO0  (D3) - MOSFET channel C
//   GPIO2  (D4) - onboard status LED, active-LOW (see STATUS_LED_PIN)
//   GPIO4  (D2) - MOSFET channel A
//   GPIO5  (D1) - MOSFET channel B
//   GPIO12 (D6) - link RX from extender board
//   GPIO14 (D5) - link TX to extender board
//   GPIO15 (D8) - MOSFET channel D
//
// Pin map (extender board, see MOSFET_EXTENDER_8266):
//   GPIO0  (D3) - MOSFET channel 2 (boot strapping pin)
//   GPIO2  (D4) - onboard status LED, active-LOW
//   GPIO4  (D2) - MOSFET channel 0
//   GPIO5  (D1) - MOSFET channel 1
//   GPIO12 (D6) - link RX from primary board
//   GPIO14 (D5) - link TX to primary board
//
// Control: a 2-channel Art-Net (DMX-over-UDP) fixture -- channel 1 is the
// shared brightness for all 7 relays (4 local + 3 on the extender), channel
// 2 selects an animated pattern from effects.h (0 = no pattern, just a flat
// fill at the channel-1 brightness). See ARTNET_CONTROL.md for how to point
// QLC+ at the board. There is no serial control interface -- USB serial is
// log output only.

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <WiFiUdp.h>

#include "effects.h"

// GPIO4 (D2) and GPIO5 (D1) each drive a MOSFET switch module directly.
static const uint8_t MOSFET_PIN_A = 4;
static const uint8_t MOSFET_PIN_B = 5;

// Third MOSFET channel. GPIO0 is a boot strapping pin (must read HIGH at
// power-on for normal boot), but that only matters during reset -- driving
// it after setup() is safe.
static const uint8_t MOSFET_PIN_C = 0; // D3

// Fourth MOSFET channel. GPIO15 is also a boot strapping pin (must read LOW
// at power-on -- the board's external pull-down handles that), so as with
// GPIO0 it's safe to drive once setup() has run.
static const uint8_t MOSFET_PIN_D = 15; // D8

// Link to a second ESP8266 ("extender" board, see MOSFET_EXTENDER_8266) that
// drives three more MOSFET channels. Runs on SoftwareSerial (not the
// hardware UART) so USB debug output keeps working over the USB cable.
// GPIO12/14 (D6/D5) are safe pins with no boot strapping behavior. Wire
// crossed: this TX (D5) -> extender RX (D6), this RX (D6) -> extender TX
// (D5), plus a shared GND between the two boards.
static const uint8_t LINK_RX_PIN = 12; // D6
static const uint8_t LINK_TX_PIN = 14; // D5
static const uint32_t LINK_BAUD = 9600;
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN);

// [0xAC sync][channel 0-2][duty 0-255] sets one extender channel's raw duty
// -- streamed continuously by the effects fade engine below as each channel
// ramps. The sync byte lets the extender resync after noise/garbage instead
// of misreading a stray byte as a command.
static const uint8_t LINK_DUTY_SYNC_BYTE = 0xAC;

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

// ---- PC audio level stream ------------------------------------------------
// A helper script on the PC (see AUDIO_LEVEL_BRIDGE/) measures how loud the
// audio going to a Windows output device is and streams it here as
// [0xAD sync][level 0-255] frames over UDP to the SoftAP below. The level
// acts as a master brightness scale multiplied into every channel's output
// (see scaleDuty(), used by the effects fade engine) at the point of write.
// audioDepthPct sets how strongly: the scale interpolates between full
// brightness (depth 0%) and the raw audio level (depth 100%). The stream is
// entirely optional -- after AUDIO_TIMEOUT_MS without a frame the scale
// snaps back to full, so the board behaves exactly as before whenever the
// script isn't running. [0xAE sync][percent] frames set audioDepthPct
// remotely (the script's --depth flag).
static const uint8_t AUDIO_LEVEL_SYNC_BYTE = 0xAD;
static const uint8_t AUDIO_DEPTH_SYNC_BYTE = 0xAE;
static const unsigned long AUDIO_TIMEOUT_MS = 1000;
static uint8_t audioLevel = 255;
static uint8_t audioDepthPct = 0;
static bool audioActive = false;
static unsigned long lastAudioRxMillis = 0;

// SoftAP the PC (and QLC+) joins to send frames without a USB cable. UDP
// rather than TCP because the audio stream is fire-and-forget: a lost
// packet just means the next one, ~16ms later, lands instead -- no
// reconnect logic to get stuck.
static const char *AP_SSID = "MULTIPLEX_LIGHTS"; // open network, no password
static const uint16_t AUDIO_UDP_PORT = 7777;
WiFiUDP audioUdp;
static bool apActive = false; // set once in setupWifi(); read by the status report

// Art-Net (DMX-over-UDP, standard port 6454) rides the same SoftAP: QLC+
// joins AP_SSID and sends ArtDMX packets addressed to this board's AP IP
// (or broadcast to the AP subnet). See the Art-Net section below.
static const uint16_t ARTNET_PORT = 6454;
WiFiUDP artnetUdp;

void onAudioLevel(uint8_t level) {
  audioLevel = level;
  lastAudioRxMillis = millis();
  if (!audioActive) {
    audioActive = true;
    Serial.printf("[AUDIO] level stream active (depth %u%%)\n", (unsigned)audioDepthPct);
  }
}

void onAudioDepth(uint8_t pct) {
  if (pct > 100) {
    pct = 100;
  }
  if (pct != audioDepthPct) { // the script re-sends every second; log changes only
    audioDepthPct = pct;
    Serial.printf("AUDIO_DEPTH_PCT = %u%% (set by stream)\n", (unsigned)pct);
  }
}

void handleAudioFrame(uint8_t sync, uint8_t value) {
  if (sync == AUDIO_LEVEL_SYNC_BYTE) {
    onAudioLevel(value);
  } else {
    onAudioDepth(value);
  }
}

// The master brightness scale right now, 0-255 (255 = no dimming).
uint8_t audioScale255() {
  if (!audioActive || audioDepthPct == 0) {
    return 255;
  }
  return (uint8_t)(255 - (uint16_t)(255 - audioLevel) * audioDepthPct / 100);
}

uint16_t scaleDuty(uint16_t duty) {
  return (uint16_t)((uint32_t)duty * audioScale255() / 255);
}

void setupWifi() {
  WiFi.persistent(false); // don't re-burn the same AP config to flash every boot
  WiFi.mode(WIFI_AP);
  apActive = WiFi.softAP(AP_SSID); // no password = open AP
  audioUdp.begin(AUDIO_UDP_PORT);
  artnetUdp.begin(ARTNET_PORT);
  if (apActive) {
    Serial.printf("[AUDIO] SoftAP '%s' (open) up -- level frames to %s:%u/udp\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str(), (unsigned)AUDIO_UDP_PORT);
    Serial.printf("[ARTNET] listening on :%u/udp -- point QLC+'s Art-Net output at %s (or broadcast)\n",
                  (unsigned)ARTNET_PORT, WiFi.softAPIP().toString().c_str());
    Serial.println("[ARTNET] once joined to the SoftAP; channel 1 = brightness, channel 2 = pattern");
  } else {
    // No serial control exists anymore -- if the AP fails to start, the
    // board is unreachable until it's power-cycled.
    Serial.println("[AUDIO/ARTNET] SoftAP failed to start -- board has no control path");
  }
}

// A packet may carry several frames (or trailing garbage), so scan for sync
// bytes rather than trusting alignment -- same spirit as the extender link.
void pollAudioUdp() {
  while (audioUdp.parsePacket() > 0) {
    uint8_t buf[64];
    int len = audioUdp.read(buf, sizeof(buf));
    for (int i = 0; i + 1 < len; i++) {
      if (buf[i] == AUDIO_LEVEL_SYNC_BYTE || buf[i] == AUDIO_DEPTH_SYNC_BYTE) {
        handleAudioFrame(buf[i], buf[i + 1]);
        i++;
      }
    }
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
// relay boards), so the standard wiring is gate-driven-HIGH = ON. No serial
// toggle anymore -- if the wiring ever needs active-LOW, flip this constant
// and reflash.
static const bool ACTIVE_LOW = false;

// Routed through analogWrite (not digitalWrite) even for instant on/off, so
// a pin that was mid-fade gets cleanly unregistered from the software PWM
// waveform instead of fighting it -- ESP8266's analogWrite treats 0 and
// PWM_MAX as "go fully static", but digitalWrite doesn't know the PWM
// waveform generator exists and won't stop it. This core's PWMRANGE isn't
// exposed as a macro, so the range is set explicitly in setup() instead of
// relying on the (255) default.
static const int PWM_MAX = 1023;

// ---- Physical light-order map -------------------------------------------
// The lamps can be hung in any physical order, independent of which pin
// drives them. channelPosition[ch] holds the physical position (0 = first
// lamp in the row) of the lamp on channel ch, with channels indexed 0-3 =
// local pins C/A/B/D (EFFECT_LOCAL_PINS), 4-6 = extender 0-2. Pattern frames
// route through this map so they sweep down the physical row even when the
// lamps aren't hung in pin order. There's no live recalibration anymore (it
// was a serial-only flow) -- this just loads whatever order was last saved
// to the EEPROM-emulated flash sector, defaulting to pin order if nothing
// valid is stored there.
static const uint8_t EFFECT_CHANNELS = 7;
static uint8_t channelPosition[EFFECT_CHANNELS] = {0, 1, 2, 3, 4, 5, 6};

static const char *CHANNEL_NAMES[EFFECT_CHANNELS] = {
    "GPIO0", "GPIO4", "GPIO5", "GPIO15", "EXT_GPIO4", "EXT_GPIO5", "EXT_GPIO0"};

// EEPROM layout: [magic 'L','O'][version][7 position bytes][checksum]. The
// checksum mixes each byte with its index so two swapped bytes can't cancel
// each other out.
static const size_t MAP_EEPROM_SIZE = 16;
static const uint8_t MAP_MAGIC_0 = 'L';
static const uint8_t MAP_MAGIC_1 = 'O';
static const uint8_t MAP_VERSION = 1;

uint8_t channelMapChecksum(const uint8_t positions[EFFECT_CHANNELS]) {
  uint8_t sum = 0x5A;
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    sum ^= (uint8_t)(positions[i] + i * 31);
  }
  return sum;
}

uint8_t channelAtPosition(uint8_t pos) {
  for (uint8_t ch = 0; ch < EFFECT_CHANNELS; ch++) {
    if (channelPosition[ch] == pos) {
      return ch;
    }
  }
  return pos; // unreachable while the map stays a permutation
}

void printChannelMap() {
  Serial.print("[MAP] light order:");
  for (uint8_t pos = 0; pos < EFFECT_CHANNELS; pos++) {
    Serial.printf(" %u:%s", (unsigned)(pos + 1), CHANNEL_NAMES[channelAtPosition(pos)]);
  }
  Serial.println();
}

// A stored map only replaces the pin-order default if the magic, version,
// and checksum all match AND the bytes form a permutation of 0-6 -- a
// half-written or stale-layout sector falls back cleanly instead of
// scrambling every pattern's output.
void loadChannelMap() {
  uint8_t stored[EFFECT_CHANNELS];
  bool valid = EEPROM.read(0) == MAP_MAGIC_0 && EEPROM.read(1) == MAP_MAGIC_1 &&
               EEPROM.read(2) == MAP_VERSION;
  if (valid) {
    for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
      stored[i] = EEPROM.read(3 + i);
    }
    valid = EEPROM.read(3 + EFFECT_CHANNELS) == channelMapChecksum(stored);
  }
  if (valid) {
    bool used[EFFECT_CHANNELS] = {};
    for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
      if (stored[i] >= EFFECT_CHANNELS || used[stored[i]]) {
        valid = false;
        break;
      }
      used[stored[i]] = true;
    }
  }
  if (valid) {
    memcpy(channelPosition, stored, EFFECT_CHANNELS);
    Serial.println("[MAP] Loaded light order from EEPROM");
  } else {
    Serial.println("[MAP] No saved light order; using pin order");
  }
  printChannelMap();
}

// duty[0..3] -> local pins in position order, duty[4..6] -> extender 0-2
static const uint8_t EFFECT_LOCAL_PINS[4] = {MOSFET_PIN_C, MOSFET_PIN_A, MOSFET_PIN_B, MOSFET_PIN_D};

// ---- Per-channel fade engine ---------------------------------------------
// Every channel tracks its own ramp toward a target duty, so brightness and
// pattern changes fade smoothly instead of snapping. setEffectTarget() sets
// a channel's target (a no-op if it's already there or already ramping
// there); updateEffectFades() walks every channel's duty toward its target
// on each ~10ms poll tick and writes it out -- locally via analogWrite,
// extender channels streamed over the [0xAC][channel][duty] link frame.
static const unsigned long EFFECT_FADE_IN_MS = 250;
static const unsigned long EFFECT_FADE_OUT_MS = 250;

struct EffectFade {
  uint16_t startDuty;   // duty when the current ramp began
  uint16_t targetDuty;  // duty the latest frame asked for
  uint16_t currentDuty; // the ramp's current position
  unsigned long startMillis;
  unsigned long durationMs;
};
static EffectFade effectFades[EFFECT_CHANNELS];

void initEffectFades() {
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    effectFades[i] = {0, 0, 0, 0, 0};
  }
}

void setEffectTarget(uint8_t channel, uint16_t target) {
  EffectFade &f = effectFades[channel];
  if (f.targetDuty == target) {
    return; // already there or already ramping there; don't restart the ramp
  }
  f.startDuty = f.currentDuty;
  f.targetDuty = target;
  f.startMillis = millis();
  f.durationMs = (target > f.currentDuty) ? EFFECT_FADE_IN_MS : EFFECT_FADE_OUT_MS;
}

// The extender's channels can't be PWM-addressed by a single frame per
// channel without cost -- a full 3-channel update is 9 bytes (~9ms of
// blocking SoftwareSerial TX at 9600 baud) and fades tick every ~10ms -- so
// writes are throttled and only-on-change.
static const unsigned long LINK_DUTY_INTERVAL_MS = 30;
static unsigned long lastLinkDutyMillis = 0;
static uint8_t lastSentExtenderDuty[3] = {0, 0, 0};

// Last level actually written to each local pin (after the audio scale), so
// a write only costs an analogWrite when the value really changed.
static uint16_t lastLocalOut[4] = {0, 0, 0, 0};

void sendLinkChannelDuty(uint8_t channel, uint8_t duty8) {
  linkSerial.write(LINK_DUTY_SYNC_BYTE);
  linkSerial.write(channel);
  linkSerial.write(duty8);
}

void updateEffectFades() {
  bool dutyWindowOpen = millis() - lastLinkDutyMillis >= LINK_DUTY_INTERVAL_MS;
  bool sentAny = false;
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    EffectFade &f = effectFades[i];
    uint16_t duty;
    unsigned long elapsed = millis() - f.startMillis;
    if (f.durationMs == 0 || elapsed >= f.durationMs) {
      duty = f.targetDuty;
    } else {
      int32_t delta = (int32_t)f.targetDuty - (int32_t)f.startDuty;
      duty = (uint16_t)((int32_t)f.startDuty + delta * (int32_t)elapsed / (int32_t)f.durationMs);
    }
    f.currentDuty = duty;
    uint16_t out = scaleDuty(duty); // audio master scale, if any
    if (i < 4) {
      if (out != lastLocalOut[i]) {
        analogWrite(EFFECT_LOCAL_PINS[i], ACTIVE_LOW ? PWM_MAX - out : out);
        lastLocalOut[i] = out;
      }
    } else {
      uint8_t duty8 = (uint8_t)((uint32_t)out * 255 / PWM_MAX);
      if (dutyWindowOpen && duty8 != lastSentExtenderDuty[i - 4]) {
        sendLinkChannelDuty(i - 4, duty8);
        lastSentExtenderDuty[i - 4] = duty8;
        sentAny = true;
      }
    }
  }
  if (sentAny) {
    lastLinkDutyMillis = millis();
  }
}

// ---- Pattern selection ----------------------------------------------------
// Channel 2 picks a pattern out of (1 + EFFECT_COUNT) equal-width DMX
// buckets: bucket 0 is "no pattern" -- every channel held at a flat fill,
// the fixture's default state -- buckets 1..EFFECT_COUNT are the ported
// QLC+ scripts from effects.h, in array order. Channel 1's brightness
// scales whichever pattern is active: for the flat fill it's the level
// directly, for an animated pattern it scales each frame's on/off duty.
static const unsigned long PATTERN_STEP_MS = 500; // animated pattern frame rate
static uint8_t effectStep = 0;
static uint8_t lastAppliedPatternIndex = 0xFF; // 0xFF = nothing applied yet

uint8_t patternIndexFromDmx(uint8_t value) {
  const uint8_t patternCount = EFFECT_COUNT + 1;
  return (uint8_t)(((uint16_t)value * patternCount) / 256);
}

// Recomputes every channel's target from the current (brightness, pattern,
// effectStep) and applies it via setEffectTarget() -- the fade engine takes
// it from there. Safe to call anytime (Art-Net frame arrival, or the
// periodic step advance in loop()); a pattern change resets effectStep so
// the new pattern always starts from its first frame.
void applyPatternFrame(uint8_t brightness, uint8_t patternRaw) {
  uint8_t pattern = patternIndexFromDmx(patternRaw);
  if (pattern != lastAppliedPatternIndex) {
    effectStep = 0;
    lastAppliedPatternIndex = pattern;
  }
  uint16_t brightnessDuty = (uint16_t)((uint32_t)brightness * PWM_MAX / 255);
  if (pattern == 0) {
    for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
      setEffectTarget(i, brightnessDuty);
    }
    return;
  }
  const Effect &fx = EFFECTS[pattern - 1];
  uint16_t duty[EFFECT_CHANNELS];
  fx.frame(EFFECT_CHANNELS, 1, effectStep, duty);
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    uint16_t raw = duty[channelPosition[i]]; // duty[] is indexed by physical position
    setEffectTarget(i, (uint16_t)((uint32_t)raw * brightnessDuty / EFFECT_DUTY_MAX));
  }
}

// ---- Art-Net DMX control ---------------------------------------------
// A minimal 2-channel ArtDMX receiver: channel 1 = brightness, channel 2 =
// pattern (see above). No universe filtering yet: whichever Art-Net
// universe QLC+ is configured to send, channels 1-2 of it are read. There's
// no timeout hand-back -- losing the Art-Net stream leaves the relays
// holding their last commanded state.
static bool artnetActive = false;
static unsigned long lastArtnetRxMillis = 0;
static uint8_t artnetBrightness = 0;
static uint8_t artnetPatternRaw = 0;

void onArtnetDmx(uint8_t brightness, uint8_t patternRaw) {
  lastArtnetRxMillis = millis();
  if (!artnetActive) {
    artnetActive = true;
    Serial.println("[ARTNET] ArtDMX stream active");
  }
  artnetBrightness = brightness;
  artnetPatternRaw = patternRaw;
  applyPatternFrame(artnetBrightness, artnetPatternRaw);
}

// Art-Net packet layout (see the Art-Net 4 spec): 8-byte ID "Art-Net\0", a
// 16-bit OpCode (little-endian), a 16-bit ProtVer (big-endian), then for
// OpDmx (0x5000): Sequence, Physical, SubUni, Net, a 16-bit Length
// (big-endian), then Length bytes of DMX data starting at channel 1. We
// only need the first two data bytes, but still validate the header so a
// stray UDP packet on this port can't be misread as a DMX frame. A packet
// with only channel 1 present (no channel 2) is still applied, with the
// pattern treated as 0 (no pattern).
static const uint16_t ARTNET_OPCODE_DMX = 0x5000;
static const size_t ARTNET_HEADER_LEN = 18; // through the two length bytes

void pollArtnetUdp() {
  while (artnetUdp.parsePacket() > 0) {
    uint8_t buf[ARTNET_HEADER_LEN + 2]; // header + DMX channels 1-2
    int len = artnetUdp.read(buf, sizeof(buf));
    if (len <= (int)ARTNET_HEADER_LEN) {
      continue; // no DMX data at all
    }
    if (memcmp(buf, "Art-Net", 7) != 0 || buf[7] != 0) {
      continue; // not an Art-Net packet
    }
    uint16_t opcode = buf[8] | ((uint16_t)buf[9] << 8);
    if (opcode != ARTNET_OPCODE_DMX) {
      continue; // ignore ArtPoll and everything else we don't need yet
    }
    uint8_t brightness = buf[ARTNET_HEADER_LEN];
    uint8_t pattern = (len > (int)ARTNET_HEADER_LEN + 1) ? buf[ARTNET_HEADER_LEN + 1] : 0;
    onArtnetDmx(brightness, pattern);
  }
}

// ---- Periodic connection status -------------------------------------------
// Every STATUS_REPORT_INTERVAL_MS, one line summarizing every link: the
// extender's serial heartbeat, how many stations have joined the SoftAP,
// and how recently the audio and Art-Net streams were heard from.
// Complements the change-triggered [LINK]/[AUDIO]/[ARTNET] logs: those say
// when something happens, this keeps saying where everything stands.
static const unsigned long STATUS_REPORT_INTERVAL_MS = 10000;
static unsigned long lastStatusReportMillis = 0;

void formatAge(char *buf, size_t len, unsigned long sinceMillis) {
  unsigned long ms = millis() - sinceMillis;
  snprintf(buf, len, "%lu.%lus ago", ms / 1000, (ms % 1000) / 100);
}

void printConnectionStatus() {
  if (millis() - lastStatusReportMillis < STATUS_REPORT_INTERVAL_MS) {
    return;
  }
  lastStatusReportMillis = millis();

  char age[24];
  Serial.print("[STATUS] extender: ");
  if (linkState == LINK_UP) {
    formatAge(age, sizeof(age), lastLinkRxMillis);
    Serial.printf("UP (heartbeat %s)", age);
  } else if (linkState == LINK_WAITING) {
    Serial.print("WAITING (no heartbeat since boot)");
  } else if (lastLinkRxMillis == 0) {
    Serial.print("DOWN (never heard from)");
  } else {
    formatAge(age, sizeof(age), lastLinkRxMillis);
    Serial.printf("DOWN (last heartbeat %s)", age);
  }

  if (apActive) {
    Serial.printf(" | AP: %u station(s)", (unsigned)WiFi.softAPgetStationNum());
  } else {
    Serial.print(" | AP: down");
  }

  if (audioActive) {
    formatAge(age, sizeof(age), lastAudioRxMillis);
    Serial.printf(" | audio: active (level %u, %s)", (unsigned)audioLevel, age);
  } else {
    Serial.print(" | audio: none");
  }

  if (artnetActive) {
    formatAge(age, sizeof(age), lastArtnetRxMillis);
    Serial.printf(" | artnet: active (brightness %u/255, pattern %u/%u, %s)",
                  (unsigned)artnetBrightness, (unsigned)patternIndexFromDmx(artnetPatternRaw),
                  (unsigned)EFFECT_COUNT, age);
  } else {
    Serial.print(" | artnet: none");
  }
  Serial.println();
}

// Same as delay(), but keeps polling the UDP streams, the link status, and
// the fade engine so they all stay responsive within ~10ms instead of
// waiting out the rest of the pattern step.
void delayWhilePolling(unsigned long ms) {
  unsigned long start = millis();
  do {
    pollAudioUdp();
    pollArtnetUdp();
    updateLinkStatus();
    printConnectionStatus();
    updateEffectFades();
    delay(10);
  } while (millis() - start < ms);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nMULTIPLEX_8266 starting...");
  Serial.println("2-channel Art-Net fixture: channel 1 = brightness, channel 2 = pattern.");
  Serial.println("See ARTNET_CONTROL.md. No serial control -- this is log output only.");

  bootFlashStatusLed();
  analogWriteRange(PWM_MAX);

  EEPROM.begin(MAP_EEPROM_SIZE);
  loadChannelMap();

  setupWifi();

  linkSerial.begin(LINK_BAUD);
  linkStateSinceMillis = millis();

  pinMode(MOSFET_PIN_A, OUTPUT);
  pinMode(MOSFET_PIN_B, OUTPUT);
  pinMode(MOSFET_PIN_C, OUTPUT);
  pinMode(MOSFET_PIN_D, OUTPUT);

  // Boots dark, with no pattern active (bucket 0) until Art-Net says
  // otherwise -- artnetBrightness/artnetPatternRaw both default to 0.
  initEffectFades();
  applyPatternFrame(artnetBrightness, artnetPatternRaw);
}

void loop() {
  uint8_t pattern = patternIndexFromDmx(artnetPatternRaw);
  if (pattern != 0) {
    const Effect &fx = EFFECTS[pattern - 1];
    effectStep = (effectStep + 1) % fx.stepCount(EFFECT_CHANNELS, 1);
  }
  applyPatternFrame(artnetBrightness, artnetPatternRaw);
  delayWhilePolling(PATTERN_STEP_MS);
}
