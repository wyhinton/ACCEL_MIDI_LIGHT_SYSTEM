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
// RELAY_PWM_8266 (see RELAY_PWM_8266) has no wire to this board at all: it
// joins this board's own SoftAP as a Wi-Fi station and the two exchange
// duty/heartbeat frames as UDP datagrams (see the "Links to satellite
// boards" section below).
//
// Control: a 15-channel Art-Net (DMX-over-UDP) fixture -- one DMX channel
// per relay (channel N = the lamp at physical position N-1, see the
// light-order map below), each an independent 0-255 brightness. Channels
// 1-7 are the 4 local MOSFETs plus the 3 on the extender board; channels
// 8-15 are the 8 PCA9685 PWM outputs on the wireless RELAY_PWM_8266 board.
// See ARTNET_CONTROL.md for how to point QLC+ at the board. There is no
// serial control interface -- USB serial is log output only.
//
// A pattern engine (effects.h ports, e.g. chases/fills) also lives in this
// file but isn't wired to Art-Net right now -- see the "Pattern selection"
// section -- kept in place rather than deleted for a quick rewire later.

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <WiFiUdp.h>
#include <stdarg.h>

#include "effects.h"

// ---- Wi-Fi debug console --------------------------------------------------
// USB serial here is log output only (see the top-of-file note) -- nothing
// ever reads bytes back from it -- so every dbgPrint/dbgPrintln/dbgPrintf
// call below is free to also mirror to a plain TCP client without stepping
// on any read protocol. A laptop already joined to the SoftAP (see AP_SSID
// further down) can then watch the log over Wi-Fi instead of needing a USB
// cable: `telnet <board-ip> 23`. Only one client at a time -- a fresh
// connection bumps whatever was already attached rather than queuing.
static const uint16_t DEBUG_TELNET_PORT = 23;
WiFiServer debugServer(DEBUG_TELNET_PORT);
WiFiClient debugClient;

void pollDebugServer() {
  if (debugServer.hasClient()) {
    if (debugClient) {
      debugClient.stop();
    }
    debugClient = debugServer.available();
    // Bounds how long a stalled/unresponsive Telnet client can block a
    // dbgPrint* call (and with it the ~10ms Art-Net/fade poll tick) --
    // default write timeout is several seconds, which would otherwise
    // stall lamp updates while nobody's even reading the log.
    debugClient.setTimeout(20);
    debugClient.println("[DEBUG] connected -- mirroring MULTIPLEX_8266 serial log");
  }
}

void dbgPrint(const char *s) {
  Serial.print(s);
  if (debugClient.connected()) {
    debugClient.print(s);
  }
}

void dbgPrintln(const char *s = "") {
  Serial.println(s);
  if (debugClient.connected()) {
    debugClient.println(s);
  }
}

void dbgPrintf(const char *fmt, ...) {
  char buf[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  if (debugClient.connected()) {
    debugClient.print(buf);
  }
}

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

// ---- Links to satellite boards --------------------------------------------
// Two satellite ESP8266 boards each add more channels: the "extender" (see
// MOSFET_EXTENDER_8266) driving three more MOSFET channels over a wired
// SoftwareSerial link (not the hardware UART, so USB debug output keeps
// working over the USB cable), and RELAY_PWM_8266 driving eight PCA9685 PWM
// channels wirelessly, as a Wi-Fi station on this board's own SoftAP. Both
// speak the same [0xAC sync][channel][duty 0-255] duty-command framing and
// send a heartbeat back on their own schedule so a LinkMonitor can tell
// whether each is actually connected -- just over different transports
// (serial bytes for the extender, UDP datagrams for RELAY_PWM_8266).
static const uint8_t LINK_RX_PIN = 12; // D6, from extender TX
static const uint8_t LINK_TX_PIN = 14; // D5, to extender RX (wire crossed, shared GND)
static const uint32_t LINK_BAUD = 9600;
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN);

static const uint8_t LINK_DUTY_SYNC_BYTE = 0xAC;

// RELAY_PWM_8266's Wi-Fi side of the link: it joins this board's SoftAP as a
// station and the two exchange UDP datagrams. Its IP isn't fixed -- DHCP
// hands it out -- so it's learned dynamically from the source address of
// its heartbeat packets rather than configured.
static const uint16_t RELAY_HEARTBEAT_PORT = 7778; // RELAY_PWM_8266 -> here
static const uint16_t RELAY_DUTY_PORT = 7779;      // here -> RELAY_PWM_8266
static const uint8_t RELAY_CHANNEL_COUNT = 8;
WiFiUDP relayUdp;
static IPAddress relayIp;
static bool relayIpKnown = false;

// Each satellite sends a heartbeat every ~250ms independent of command
// traffic. We don't care what arrives, only that something does. A monitor
// starts in WAITING rather than assuming "down", so the first
// LINK_TIMEOUT_MS after boot (while the satellite is still starting up)
// doesn't immediately read the same as a real disconnect -- but WAITING
// still times out into a logged DOWN if nothing ever arrives, so silence
// always ends up reported instead of never being logged at all. While DOWN,
// we're still listening for the satellite to show back up, so keep
// re-announcing every LINK_STATUS_REPEAT_MS instead of logging once and
// going quiet.
static const unsigned long LINK_TIMEOUT_MS = 1000;
static const unsigned long LINK_STATUS_REPEAT_MS = 2000;
enum LinkState { LINK_WAITING, LINK_UP, LINK_DOWN };

struct LinkMonitor {
  const char *name;
  LinkState state = LINK_WAITING;
  unsigned long stateSinceMillis = 0; // set in setup()
  unsigned long lastRxMillis = 0;
  unsigned long lastStatusPrintMillis = 0;
};
static LinkMonitor extenderLinkMonitor{"Extender"};
static LinkMonitor relayLinkMonitor{"RELAY_PWM_8266"};

// Updates one satellite's up/down state machine given whether contact (a
// heartbeat byte or packet) was seen since the last call -- transport-
// agnostic so the same logic covers the extender's serial heartbeat and
// RELAY_PWM_8266's UDP one.
void updateLinkMonitor(LinkMonitor &m, bool gotContact) {
  if (gotContact) {
    m.lastRxMillis = millis();
    if (m.state != LINK_UP) {
      m.state = LINK_UP;
      dbgPrintf("[LINK] %s connected\n", m.name);
    }
    return;
  }

  unsigned long referenceMillis = (m.state == LINK_WAITING) ? m.stateSinceMillis : m.lastRxMillis;
  unsigned long downForMs = millis() - referenceMillis;
  if (downForMs < LINK_TIMEOUT_MS) {
    return;
  }

  bool justWentDown = (m.state != LINK_DOWN);
  if (justWentDown || millis() - m.lastStatusPrintMillis >= LINK_STATUS_REPEAT_MS) {
    m.state = LINK_DOWN;
    m.lastStatusPrintMillis = millis();
    dbgPrintf("[LINK] %s disconnected (no heartbeat for %lus), listening for reconnect...\n", m.name, downForMs / 1000);
  }
}

// Forces every RELAY_PWM_8266 channel to be resent next fade tick regardless
// of whether its duty actually changed -- needed because that board zeroes
// its own outputs on boot, and our send-only-on-change throttle otherwise
// has no reason to repeat whatever duty was already current when it
// (re)joined, leaving it stuck dark until the next real Art-Net change.
// Defined near updateEffectFades(); forward-declared here since
// updateLinkStatus() runs the reconnect check that calls it.
void resendAllRelayDuty();

void updateLinkStatus() {
  bool extenderGotByte = false;
  while (linkSerial.available()) {
    linkSerial.read();
    extenderGotByte = true;
  }
  updateLinkMonitor(extenderLinkMonitor, extenderGotByte);

  bool relayGotPacket = false;
  while (relayUdp.parsePacket() > 0) {
    uint8_t discard[8];
    relayUdp.read(discard, sizeof(discard));
    relayIp = relayUdp.remoteIP();
    relayIpKnown = true;
    relayGotPacket = true;
  }
  bool relayWasUp = (relayLinkMonitor.state == LINK_UP);
  updateLinkMonitor(relayLinkMonitor, relayGotPacket);
  if (relayGotPacket && !relayWasUp) {
    dbgPrintln("[LINK] RELAY_PWM_8266 (re)joined -- resending current channel state");
    resendAllRelayDuty();
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
    dbgPrintf("[AUDIO] level stream active (depth %u%%)\n", (unsigned)audioDepthPct);
  }
}

void onAudioDepth(uint8_t pct) {
  if (pct > 100) {
    pct = 100;
  }
  if (pct != audioDepthPct) { // the script re-sends every second; log changes only
    audioDepthPct = pct;
    dbgPrintf("AUDIO_DEPTH_PCT = %u%% (set by stream)\n", (unsigned)pct);
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
  relayUdp.begin(RELAY_HEARTBEAT_PORT);
  debugServer.begin();
  debugServer.setNoDelay(true);
  if (apActive) {
    dbgPrintf("[AUDIO] SoftAP '%s' (open) up -- level frames to %s:%u/udp\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str(), (unsigned)AUDIO_UDP_PORT);
    dbgPrintf("[ARTNET] listening on :%u/udp -- point QLC+'s Art-Net output at %s (or broadcast)\n",
                  (unsigned)ARTNET_PORT, WiFi.softAPIP().toString().c_str());
    dbgPrintln("[ARTNET] once joined to the SoftAP; channels 1-15 = one brightness per relay");
    dbgPrintf("[RELAY] waiting for RELAY_PWM_8266 to join the SoftAP and heartbeat on :%u/udp\n",
                  (unsigned)RELAY_HEARTBEAT_PORT);
    dbgPrintf("[DEBUG] telnet mirror on :%u -- run: telnet %s %u\n",
                  (unsigned)DEBUG_TELNET_PORT, WiFi.softAPIP().toString().c_str(), (unsigned)DEBUG_TELNET_PORT);
  } else {
    // No serial control exists anymore -- if the AP fails to start, the
    // board is unreachable until it's power-cycled.
    dbgPrintln("[AUDIO/ARTNET] SoftAP failed to start -- board has no control path");
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
// then toggles once per incoming Art-Net packet (see onArtnetDmx()) as a
// link-activity indicator -- solid and unchanging means no fresh DMX has
// arrived since the last toggle, flickering means the Wi-Fi link is
// actually delivering updates.
static const uint8_t STATUS_LED_PIN = 2;
static bool statusLedOn = true; // matches the solid-on state bootFlashStatusLed() leaves it in

void bootFlashStatusLed() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  unsigned long start = millis();
  bool on = false;
  while (millis() - start < 1000) {
    on = !on;
    digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
    delay(50);
  }
  digitalWrite(STATUS_LED_PIN, LOW); // stays lit until the first Art-Net packet toggles it
}

void toggleStatusLed() {
  statusLedOn = !statusLedOn;
  digitalWrite(STATUS_LED_PIN, statusLedOn ? LOW : HIGH);
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
// local pins C/A/B/D (EFFECT_LOCAL_PINS), 4-6 = extender 0-2, 7-14 =
// RELAY_PWM_8266 0-7. Pattern frames route through this map so they sweep
// down the physical row even when the lamps aren't hung in pin order.
// There's no live recalibration anymore (it was a serial-only flow) --
// this just loads whatever order was last saved to the EEPROM-emulated
// flash sector, defaulting to pin order if nothing valid is stored there.
static const uint8_t EFFECT_CHANNELS = 15;
static uint8_t channelPosition[EFFECT_CHANNELS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

static const char *CHANNEL_NAMES[EFFECT_CHANNELS] = {
    "GPIO0",      "GPIO4",      "GPIO5",      "GPIO15",     "EXT_GPIO4",
    "EXT_GPIO5",  "EXT_GPIO0",  "RELAY_0",    "RELAY_1",    "RELAY_2",
    "RELAY_3",    "RELAY_4",    "RELAY_5",    "RELAY_6",    "RELAY_7"};

// EEPROM layout: [magic 'L','O'][version][15 position bytes][checksum]. The
// checksum mixes each byte with its index so two swapped bytes can't cancel
// each other out.
static const size_t MAP_EEPROM_SIZE = 32;
static const uint8_t MAP_MAGIC_0 = 'L';
static const uint8_t MAP_MAGIC_1 = 'O';
// Bumped from 1: the stored layout grew from 7 to 15 position bytes, so a
// sector written by the old 7-channel firmware must not be misread as valid.
static const uint8_t MAP_VERSION = 2;

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
  dbgPrint("[MAP] light order:");
  for (uint8_t pos = 0; pos < EFFECT_CHANNELS; pos++) {
    dbgPrintf(" %u:%s", (unsigned)(pos + 1), CHANNEL_NAMES[channelAtPosition(pos)]);
  }
  dbgPrintln();
}

// A stored map only replaces the pin-order default if the magic, version,
// and checksum all match AND the bytes form a permutation of 0-14 -- a
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
    dbgPrintln("[MAP] Loaded light order from EEPROM");
  } else {
    dbgPrintln("[MAP] No saved light order; using pin order");
  }
  printChannelMap();
}

// duty[0..3] -> local pins in position order, duty[4..6] -> extender 0-2,
// duty[7..14] -> RELAY_PWM_8266 0-7
static const uint8_t EFFECT_LOCAL_PINS[4] = {MOSFET_PIN_C, MOSFET_PIN_A, MOSFET_PIN_B, MOSFET_PIN_D};

// ---- Per-channel fade engine ---------------------------------------------
// Every channel tracks its own ramp toward a target duty, so brightness and
// pattern changes fade smoothly instead of snapping. setEffectTarget() sets
// a channel's target (a no-op if it's already there or already ramping
// there); updateEffectFades() walks every channel's duty toward its target
// on each ~10ms poll tick and writes it out -- locally via analogWrite, the
// extender's and RELAY_PWM_8266's channels each streamed over their own
// [0xAC][channel][duty] link frame.
static const unsigned long EFFECT_FADE_IN_MS = 0;
static const unsigned long EFFECT_FADE_OUT_MS = 0;

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

// Neither satellite's channels can be PWM-addressed by a single frame per
// channel without cost -- e.g. a full 3-channel extender update is 9 bytes
// (~9ms of blocking SoftwareSerial TX at 9600 baud), and every RELAY_PWM_8266
// channel change is its own UDP packet -- so writes are throttled and
// only-on-change. lastSentRelayDuty uses -1 (never a real 0-255 duty) as
// "unknown" so resendAllRelayDuty() (see updateLinkStatus()) can force a
// resend without needing a separate "known" flag per channel.
static const unsigned long LINK_DUTY_INTERVAL_MS = 30;
static unsigned long lastLinkDutyMillis = 0;
static uint8_t lastSentExtenderDuty[3] = {0, 0, 0};
static int16_t lastSentRelayDuty[RELAY_CHANNEL_COUNT] = {-1, -1, -1, -1, -1, -1, -1, -1};

// Last level actually written to each local pin (after the audio scale), so
// a write only costs an analogWrite when the value really changed.
static uint16_t lastLocalOut[4] = {0, 0, 0, 0};

void resendAllRelayDuty() {
  for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
    lastSentRelayDuty[i] = -1;
  }
}

// Sends [0xAC][channel][duty8] to the extender over its serial link if
// duty8 actually changed since the last send.
void sendExtenderChannelDutyIfChanged(uint8_t channel, uint8_t duty8, bool dutyWindowOpen, bool &sentAny) {
  if (!dutyWindowOpen || duty8 == lastSentExtenderDuty[channel]) {
    return;
  }
  linkSerial.write(LINK_DUTY_SYNC_BYTE);
  linkSerial.write(channel);
  linkSerial.write(duty8);
  lastSentExtenderDuty[channel] = duty8;
  sentAny = true;
}

// Same [0xAC][channel][duty8] framing, sent as a UDP datagram to
// RELAY_PWM_8266 instead of serial bytes. A silent no-op until relayIp is
// known (it hasn't sent its first heartbeat yet); resendAllRelayDuty()
// catches it up once it does.
void sendRelayChannelDutyIfChanged(uint8_t channel, uint8_t duty8, bool dutyWindowOpen, bool &sentAny) {
  if (!relayIpKnown || !dutyWindowOpen || (int16_t)duty8 == lastSentRelayDuty[channel]) {
    return;
  }
  relayUdp.beginPacket(relayIp, RELAY_DUTY_PORT);
  relayUdp.write(LINK_DUTY_SYNC_BYTE);
  relayUdp.write(channel);
  relayUdp.write(duty8);
  relayUdp.endPacket();
  lastSentRelayDuty[channel] = duty8;
  sentAny = true;
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
    } else if (i < 7) {
      uint8_t duty8 = (uint8_t)((uint32_t)out * 255 / PWM_MAX);
      sendExtenderChannelDutyIfChanged(i - 4, duty8, dutyWindowOpen, sentAny);
    } else {
      uint8_t duty8 = (uint8_t)((uint32_t)out * 255 / PWM_MAX);
      sendRelayChannelDutyIfChanged(i - 7, duty8, dutyWindowOpen, sentAny);
    }
  }
  if (sentAny) {
    lastLinkDutyMillis = millis();
  }
}

// ---- Pattern selection (not currently wired to Art-Net) -------------------
// NOT CALLED FROM loop()/onArtnetDmx() RIGHT NOW -- the active control path
// is the direct one-DMX-channel-per-relay scheme below instead. Kept intact
// (rather than deleted) so it's a quick rewire, not a rewrite, if/when we
// want a pattern-select channel again: a value 0..255 would pick a pattern
// out of (1 + EFFECT_COUNT) equal-width DMX buckets -- bucket 0 "no
// pattern" (a flat fill), buckets 1..EFFECT_COUNT the ported QLC+ scripts
// from effects.h in array order -- with a second channel's brightness
// scaling whichever pattern is active.
static const unsigned long PATTERN_STEP_MS = 500; // animated pattern frame rate, if re-enabled
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
// A minimal ArtDMX receiver, one channel per relay: DMX channel N (1-based)
// is the lamp at physical position N-1 -- i.e. it goes through the same
// light-order map as everything else, so channel 1 is always "the first
// lamp in the row" regardless of which pin actually drives it. No universe
// filtering yet: whichever Art-Net universe QLC+ is configured to send,
// channels 1-15 of it are read. There's no timeout hand-back -- losing the
// Art-Net stream leaves the relays holding their last commanded state.
static bool artnetActive = false;
static unsigned long lastArtnetRxMillis = 0;
static uint8_t artnetLevels[EFFECT_CHANNELS] = {0}; // DMX channels 1-15, by position

// Applies one 0-255 level per physical position directly to the fade
// engine -- the pattern engine above is bypassed entirely on this path.
void applyDirectLevels(const uint8_t levels[EFFECT_CHANNELS]) {
  for (uint8_t pos = 0; pos < EFFECT_CHANNELS; pos++) {
    uint8_t ch = channelAtPosition(pos);
    uint16_t target = (uint16_t)((uint32_t)levels[pos] * PWM_MAX / 255);
    setEffectTarget(ch, target);
  }
}

void onArtnetDmx(const uint8_t levels[EFFECT_CHANNELS]) {
  lastArtnetRxMillis = millis();
  toggleStatusLed();
  if (!artnetActive) {
    artnetActive = true;
    dbgPrintln("[ARTNET] ArtDMX stream active");
  }
  memcpy(artnetLevels, levels, EFFECT_CHANNELS);
  applyDirectLevels(artnetLevels);
}

// Art-Net packet layout (see the Art-Net 4 spec): 8-byte ID "Art-Net\0", a
// 16-bit OpCode (little-endian), a 16-bit ProtVer (big-endian), then for
// OpDmx (0x5000): Sequence, Physical, SubUni, Net, a 16-bit Length
// (big-endian), then Length bytes of DMX data starting at channel 1. We
// only need the first EFFECT_CHANNELS data bytes, but still validate the
// header so a stray UDP packet on this port can't be misread as a DMX
// frame. A packet shorter than 15 channels is still applied, with whichever
// trailing channels are missing treated as 0 (off).
static const uint16_t ARTNET_OPCODE_DMX = 0x5000;
static const size_t ARTNET_HEADER_LEN = 18; // through the two length bytes

void pollArtnetUdp() {
  while (artnetUdp.parsePacket() > 0) {
    uint8_t buf[ARTNET_HEADER_LEN + EFFECT_CHANNELS]; // header + DMX channels 1-15
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
    uint8_t levels[EFFECT_CHANNELS] = {0};
    int available = len - (int)ARTNET_HEADER_LEN;
    if (available > EFFECT_CHANNELS) {
      available = EFFECT_CHANNELS;
    }
    for (int i = 0; i < available; i++) {
      levels[i] = buf[ARTNET_HEADER_LEN + i];
    }
    onArtnetDmx(levels);
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

void printLinkStatusSegment(const LinkMonitor &m) {
  char age[24];
  dbgPrintf("%s: ", m.name);
  if (m.state == LINK_UP) {
    formatAge(age, sizeof(age), m.lastRxMillis);
    dbgPrintf("UP (heartbeat %s)", age);
  } else if (m.state == LINK_WAITING) {
    dbgPrint("WAITING (no heartbeat since boot)");
  } else if (m.lastRxMillis == 0) {
    dbgPrint("DOWN (never heard from)");
  } else {
    formatAge(age, sizeof(age), m.lastRxMillis);
    dbgPrintf("DOWN (last heartbeat %s)", age);
  }
}

void printConnectionStatus() {
  if (millis() - lastStatusReportMillis < STATUS_REPORT_INTERVAL_MS) {
    return;
  }
  lastStatusReportMillis = millis();

  char age[24];
  dbgPrint("[STATUS] ");
  printLinkStatusSegment(extenderLinkMonitor);
  dbgPrint(" | ");
  printLinkStatusSegment(relayLinkMonitor);

  if (apActive) {
    dbgPrintf(" | AP: %u station(s)", (unsigned)WiFi.softAPgetStationNum());
  } else {
    dbgPrint(" | AP: down");
  }

  if (audioActive) {
    formatAge(age, sizeof(age), lastAudioRxMillis);
    dbgPrintf(" | audio: active (level %u, %s)", (unsigned)audioLevel, age);
  } else {
    dbgPrint(" | audio: none");
  }

  if (artnetActive) {
    formatAge(age, sizeof(age), lastArtnetRxMillis);
    dbgPrint(" | artnet: active (levels");
    for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
      dbgPrintf(" %u", (unsigned)artnetLevels[i]);
    }
    dbgPrintf(", %s)", age);
  } else {
    dbgPrint(" | artnet: none");
  }
  dbgPrintln();
}

// Same as delay(), but keeps polling the UDP streams, the link status, and
// the fade engine so they all stay responsive within ~10ms instead of
// waiting out the rest of the pattern step.
void delayWhilePolling(unsigned long ms) {
  unsigned long start = millis();
  do {
    pollDebugServer();
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
  dbgPrintln("\nMULTIPLEX_8266 starting...");
  dbgPrintln("15-channel Art-Net fixture: one brightness channel per relay.");
  dbgPrintln("See ARTNET_CONTROL.md. No serial control -- this is log output only.");

  bootFlashStatusLed();
  analogWriteRange(PWM_MAX);

  EEPROM.begin(MAP_EEPROM_SIZE);
  loadChannelMap();

  setupWifi();

  linkSerial.begin(LINK_BAUD);
  unsigned long linkBootMillis = millis();
  extenderLinkMonitor.stateSinceMillis = linkBootMillis;
  relayLinkMonitor.stateSinceMillis = linkBootMillis;

  pinMode(MOSFET_PIN_A, OUTPUT);
  pinMode(MOSFET_PIN_B, OUTPUT);
  pinMode(MOSFET_PIN_C, OUTPUT);
  pinMode(MOSFET_PIN_D, OUTPUT);

  // Boots dark -- every relay off until Art-Net says otherwise
  // (artnetLevels defaults to all zero).
  initEffectFades();
  applyDirectLevels(artnetLevels);
}

// Control is entirely event-driven now (onArtnetDmx() retargets the fade
// engine as soon as a packet arrives), so loop() just needs to keep the
// UDP/link/fade polling alive -- the tick length here doesn't affect
// responsiveness, that's delayWhilePolling()'s ~10ms inner poll.
static const unsigned long POLL_TICK_MS = 50;

void loop() {
  delayWhilePolling(POLL_TICK_MS);
}
