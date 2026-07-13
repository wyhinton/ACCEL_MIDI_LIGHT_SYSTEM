// Pin map (primary board):
//   GPIO0  (D3) - MOSFET channel C, chase step 1 (boot strapping pin, see MOSFET_PIN_C)
//   GPIO2  (D4) - onboard status LED, active-LOW (see STATUS_LED_PIN)
//   GPIO4  (D2) - MOSFET channel A, chase step 2
//   GPIO5  (D1) - MOSFET channel B, chase step 3
//   GPIO12 (D6) - link RX from extender board
//   GPIO14 (D5) - link TX to extender board
//   GPIO15 (D8) - MOSFET channel D, chase step 4 (boot strapping pin, see MOSFET_PIN_D)
//
// Pin map (extender board, see MOSFET_EXTENDER_8266):
//   GPIO0  (D3) - MOSFET channel 2, chase step 7 (boot strapping pin)
//   GPIO2  (D4) - onboard status LED, active-LOW
//   GPIO4  (D2) - MOSFET channel 0, chase step 5
//   GPIO5  (D1) - MOSFET channel 1, chase step 6
//   GPIO12 (D6) - link RX from primary board
//   GPIO14 (D5) - link TX to primary board

#include <Arduino.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>

#include "effects.h"

// Serial key commands (see handleSerial()):
//   0/1  active-HIGH (default) / active-LOW polarity
//   2    toggle PWM fade-in vs instant switching, this board + the extender
//   +/-  adjust fade duration (50ms per press, clamped 50-2000ms);
//        in test-all mode adjusts the shared brightness instead, and in
//        effects mode adjusts the effect fade-in time (1ms per press
//        unshifted '='/'-', 10ms shifted '+'/'_')
//   ./,  adjust the effects fade-out time (1ms per press, clamped
//        0-2000ms, 0 = instant); shifted ('>'/'<') moves 10ms per press
//   ]/[  adjust chase step length (50ms per press, clamped 1-1000ms);
//        also sets the frame length in effects test mode
//   b    toggle blackout (forces every channel off, including the extender's)
//   t    toggle test-all mode (every channel on at a dim shared brightness)
//   e    toggle effects test mode (QLC+ effect ports from effects.h across
//        all 7 channels, each channel ramping per the effect fade-in/out
//        times); while active, 'n'/'p' picks the next/previous effect
//   m    calibrate the physical light order: each channel lights alone in
//        turn and you type the lit lamp's position 1-7; once all seven are
//        assigned the map is saved to EEPROM flash ('m' mid-run aborts)
//   Effects noise (see updateEffectNoise()) -- how deeply the per-channel
//   wandering noise randomizes the effect, each its own 0-100% depth:
//   a/z  raise/lower the brightness depth (lit channels shimmer dimmer)
//   s/x  raise/lower the fade-in depth (each ramp-up scaled 0..2x)
//   d/c  raise/lower the fade-out depth (same, for ramp-downs)
//   f/v  slow down / speed up the noise itself (100ms per press,
//        100-5000ms per new random sample)

// GPIO4 (D2) and GPIO5 (D1) each drive a MOSFET switch module directly --
// no more PCF8574 I2C expander / multiplexer. Part of a chase sequence, see
// chaseStepMs / applyChaseStep() below.
static const uint8_t MOSFET_PIN_A = 4;
static const uint8_t MOSFET_PIN_B = 5;

// Third MOSFET channel, in phase with A (on/off together). GPIO0 is a boot
// strapping pin (must read HIGH at power-on for normal boot), but that only
// matters during reset -- driving it after setup() is safe.
static const uint8_t MOSFET_PIN_C = 0; // D3

// Fourth MOSFET channel. GPIO15 is also a boot strapping pin (must read LOW
// at power-on -- the board's external pull-down handles that), so as with
// GPIO0 it's safe to drive once setup() has run.
static const uint8_t MOSFET_PIN_D = 15; // D8

// Link to a second ESP8266 ("extender" board, see MOSFET_EXTENDER_8266) that
// drives three more MOSFET channels. Runs on SoftwareSerial (not the hardware
// UART) so USB debug output and the '1'/'0' active-low toggle below keep
// working over the USB cable. GPIO12/14 (D6/D5) are safe pins with no boot
// strapping behavior. Wire crossed: this TX (D5) -> extender RX (D6), this
// RX (D6) -> extender TX (D5), plus a shared GND between the two boards.
static const uint8_t LINK_RX_PIN = 12; // D6
static const uint8_t LINK_TX_PIN = 14; // D5
static const uint32_t LINK_BAUD = 9600;
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN);

// Frames sent to the extender board: [0xAA sync][cmd] for per-channel
// on/off -- cmd bit0 = state (1=on, 0=off), bits1-2 = channel (0-2), bit3 =
// fade mode (see fadeMode below) -- [0xAB sync][brightness 0-255] to drive
// every extender channel to the same raw duty (test-all mode, see
// applyTestAll()) -- or [0xAC sync][channel 0-2][duty 0-255] to set one
// channel's raw duty (streamed by the effects fade engine, see
// updateEffectFades()). The sync byte lets the extender resync after
// noise/garbage instead of misreading a stray byte as a command.
static const uint8_t LINK_SYNC_BYTE = 0xAA;
static const uint8_t LINK_BRIGHTNESS_SYNC_BYTE = 0xAB;
static const uint8_t LINK_DUTY_SYNC_BYTE = 0xAC;

void sendLinkCommand(uint8_t channel, bool on, bool fade) {
  uint8_t cmd = (fade ? 0x8 : 0) | (channel << 1) | (on ? 1 : 0);
  linkSerial.write(LINK_SYNC_BYTE);
  linkSerial.write(cmd);
}

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
// relay boards), so the standard wiring is gate-driven-HIGH = ON.
// Toggle live over serial instead of reflashing: send '1' for active-LOW,
// '0' for active-HIGH (default).
static bool activeLow = false;

// Routed through analogWrite (not digitalWrite) even for instant on/off, so
// a pin that was mid-fade gets cleanly unregistered from the software PWM
// waveform instead of fighting it -- ESP8266's analogWrite treats 0 and
// PWM_MAX as "go fully static", but digitalWrite doesn't know the PWM
// waveform generator exists and won't stop it. This core's PWMRANGE isn't
// exposed as a macro, so the range is set explicitly in setup() instead of
// relying on the (255) default.
static const int PWM_MAX = 1023;

void setMosfet(uint8_t pin, bool on) {
  bool level = activeLow ? !on : on;
  analogWrite(pin, level ? PWM_MAX : 0);
}

// Press '2' to toggle between the chase instantly switching each relay and
// ramping it in with PWM instead. Applies to this board's local channels
// and the extender's -- the fade bit rides along in every link command.
static bool fadeMode = false;

// When fadeMode is on, the pin that just turned "on" ramps 0 -> PWM_MAX
// over fadeDurationMs, then holds at full for the rest of its chase step.
// Only one local pin is ever "on" at a time in this chase, so only one fade
// runs at once; turning a pin off cancels whatever fade was in progress.
// Adjustable live over serial with '+'/'-' (see adjustFadeDuration()) so a
// change takes effect immediately, even mid-ramp.
static unsigned long fadeDurationMs = 250;
static const unsigned long FADE_DURATION_STEP_MS = 50;
static const unsigned long FADE_DURATION_MIN_MS = 50;
static const unsigned long FADE_DURATION_MAX_MS = 2000;
static uint8_t fadingPin = 255; // 255 = no fade in progress
static unsigned long fadeStartMillis = 0;

void beginFade(uint8_t pin) {
  fadingPin = pin;
  fadeStartMillis = millis();
  analogWrite(pin, activeLow ? PWM_MAX : 0);
}

void cancelFade(uint8_t pin) {
  if (fadingPin == pin) {
    fadingPin = 255;
  }
}

void updateFade() {
  if (fadingPin == 255) {
    return;
  }
  unsigned long elapsed = millis() - fadeStartMillis;
  if (elapsed >= fadeDurationMs) {
    analogWrite(fadingPin, activeLow ? 0 : PWM_MAX);
    fadingPin = 255;
    return;
  }
  int duty = (int)((uint32_t)elapsed * PWM_MAX / fadeDurationMs);
  analogWrite(fadingPin, activeLow ? (PWM_MAX - duty) : duty);
}

void adjustFadeDuration(long deltaMs) {
  long updated = (long)fadeDurationMs + deltaMs;
  if (updated < (long)FADE_DURATION_MIN_MS) {
    updated = FADE_DURATION_MIN_MS;
  } else if (updated > (long)FADE_DURATION_MAX_MS) {
    updated = FADE_DURATION_MAX_MS;
  }
  fadeDurationMs = (unsigned long)updated;
  Serial.printf("FADE_DURATION_MS = %lu\n", fadeDurationMs);
}

void applyLocalPin(uint8_t pin, bool on) {
  if (on) {
    if (fadeMode) {
      beginFade(pin);
    } else {
      setMosfet(pin, true);
    }
  } else {
    cancelFade(pin);
    setMosfet(pin, false);
  }
}

// Press 'b' to toggle a manual blackout: forces every channel off,
// including the extender's, and holds the chase paused until pressed
// again. Applied the instant the key is read (inside delayWithSerial's
// ~10ms poll), not on the next chase step boundary, so it can't be stuck
// waiting out whatever's mid-fade.
static bool blackout = false;

void applyBlackout() {
  applyLocalPin(MOSFET_PIN_C, false);
  applyLocalPin(MOSFET_PIN_A, false);
  applyLocalPin(MOSFET_PIN_B, false);
  applyLocalPin(MOSFET_PIN_D, false);
  sendLinkCommand(0, false, false);
  sendLinkCommand(1, false, false);
  sendLinkCommand(2, false, false);
}

// Press 't' to toggle a test-all mode: every channel on at once (including
// the extender's, via the 0xAB brightness frame) at a dim starting
// brightness, with '+'/'-' repurposed to raise/lower that shared brightness
// while the mode is active. The chase pauses until 't' is pressed again.
static bool testAllMode = false;
static int testBrightness = 0;
static const int TEST_BRIGHTNESS_INITIAL = 32;
static const int TEST_BRIGHTNESS_STEP = 32;

void sendLinkBrightness(int duty) {
  linkSerial.write(LINK_BRIGHTNESS_SYNC_BYTE);
  linkSerial.write((uint8_t)((long)duty * 255 / PWM_MAX));
}

void applyTestAll() {
  fadingPin = 255; // direct duty writes below; don't let a stale fade fight them
  int level = activeLow ? PWM_MAX - testBrightness : testBrightness;
  analogWrite(MOSFET_PIN_C, level);
  analogWrite(MOSFET_PIN_A, level);
  analogWrite(MOSFET_PIN_B, level);
  analogWrite(MOSFET_PIN_D, level);
  sendLinkBrightness(testBrightness);
}

void adjustTestBrightness(int delta) {
  int updated = testBrightness + delta;
  if (updated < 0) {
    updated = 0;
  } else if (updated > PWM_MAX) {
    updated = PWM_MAX;
  }
  testBrightness = updated;
  Serial.printf("TEST_BRIGHTNESS = %d / %d\n", testBrightness, PWM_MAX);
  applyTestAll();
}

// Press 'e' to toggle effects test mode: runs the QLC+ script ports from
// effects.h across all 7 channels (4 local + 3 extender) as a 7x1 strip,
// one frame per chaseStepMs (']'/'[' adjusts as usual), with 'n'/'p'
// selecting the next/previous effect. The chase pauses until 'e' again.
static bool effectsMode = false;
static uint8_t effectIndex = 0;
static uint16_t effectStep = 0;
static const uint8_t EFFECT_CHANNELS = 7;

// duty[0..3] -> local pins in chase order, duty[4..6] -> extender 0-2
static const uint8_t EFFECT_LOCAL_PINS[4] = {MOSFET_PIN_C, MOSFET_PIN_A, MOSFET_PIN_B, MOSFET_PIN_D};

// ---- Physical light-order map -------------------------------------------
// The lamps can be hung in any physical order, independent of which pin
// drives them. channelPosition[ch] holds the physical position (0 = first
// lamp in the row) of the lamp on channel ch, with channels indexed as in
// effects mode: 0-3 = local pins C/A/B/D (EFFECT_LOCAL_PINS), 4-6 =
// extender 0-2. Both order-aware consumers -- the chase and the effect
// frames -- light physical positions and route through this map to reach
// pins. Defaults to pin order; recalibrate live with the 'm' serial
// command (below), which persists the result in the ESP8266's
// EEPROM-emulated flash sector so it survives reboots and reflashes.
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
// scrambling every mode's output.
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
    Serial.println("[MAP] No saved light order; using pin order ('m' calibrates)");
  }
  printChannelMap();
}

void saveChannelMap() {
  EEPROM.write(0, MAP_MAGIC_0);
  EEPROM.write(1, MAP_MAGIC_1);
  EEPROM.write(2, MAP_VERSION);
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    EEPROM.write(3 + i, channelPosition[i]);
  }
  EEPROM.write(3 + EFFECT_CHANNELS, channelMapChecksum(channelPosition));
  if (EEPROM.commit()) {
    Serial.println("[MAP] Light order saved to EEPROM");
  } else {
    Serial.println("[MAP] EEPROM commit FAILED -- order active this session but not persisted");
  }
}

// Press 'm' to calibrate: each channel lights alone in turn, and you type
// the digit 1-7 of where the lit lamp sits in the physical row (1 = first
// lamp). Assignments accumulate in calibrateAssign so an abort ('m' again)
// keeps the previous map untouched; once all seven are assigned the new map
// is adopted and saved. While active, calibration swallows every key (see
// handleSerial) so the digits can't trip the polarity/fade toggles.
static bool calibrateMode = false;
static uint8_t calibrateChannel = 0;
static uint8_t calibrateAssign[EFFECT_CHANNELS];

void calibrateDrive(uint8_t channel, bool on) {
  if (channel < 4) {
    setMosfet(EFFECT_LOCAL_PINS[channel], on); // instant on purpose; ignores fadeMode
  } else {
    sendLinkCommand(channel - 4, on, false);
  }
}

void calibratePrompt() {
  calibrateDrive(calibrateChannel, true);
  Serial.printf("[MAP] Channel %u/%u (%s) is lit -- type its position 1-%u (1 = first lamp; 'm' aborts)\n",
                (unsigned)(calibrateChannel + 1), (unsigned)EFFECT_CHANNELS,
                CHANNEL_NAMES[calibrateChannel], (unsigned)EFFECT_CHANNELS);
}

void beginCalibrate() {
  calibrateMode = true;
  calibrateChannel = 0;
  applyBlackout(); // known-dark so the one lit lamp is unambiguous
  Serial.println("CALIBRATE = true");
  calibratePrompt();
}

void endCalibrate(bool adopt) {
  calibrateMode = false;
  applyBlackout(); // everything off; the chase re-lights on its next step
  if (adopt) {
    memcpy(channelPosition, calibrateAssign, EFFECT_CHANNELS);
    saveChannelMap();
  } else {
    Serial.println("[MAP] Calibration aborted; previous light order kept");
  }
  printChannelMap();
}

void handleCalibrateKey(char c) {
  if (c == 'm') {
    endCalibrate(false);
    return;
  }
  if (c >= '1' && c < (char)('1' + EFFECT_CHANNELS)) {
    uint8_t pos = (uint8_t)(c - '1');
    for (uint8_t ch = 0; ch < calibrateChannel; ch++) {
      if (calibrateAssign[ch] == pos) {
        Serial.printf("[MAP] Position %c is already %s -- pick another\n", c, CHANNEL_NAMES[ch]);
        return;
      }
    }
    calibrateAssign[calibrateChannel] = pos;
    Serial.printf("[MAP] %s -> position %c\n", CHANNEL_NAMES[calibrateChannel], c);
    calibrateDrive(calibrateChannel, false);
    calibrateChannel++;
    if (calibrateChannel >= EFFECT_CHANNELS) {
      endCalibrate(true);
    } else {
      calibratePrompt();
    }
    return;
  }
  if (c != '\r' && c != '\n' && c != '\t' && c != ' ' && c != 0) {
    Serial.println("[MAP] (calibrating: digits 1-7 assign the lit lamp's position, 'm' aborts)");
  }
}

// Per-channel fade engine for effects mode. Unlike the chase's single
// fadingPin engine, every channel tracks its own ramp, so one frame can
// fade several channels in and out at once. applyEffectFrame() sets
// targets; updateEffectFades() walks each channel's duty toward its target
// on every ~10ms delayWithSerial() tick. A channel that keeps the same
// target across frames holds its level (or ramp) untouched. Fade-in and
// fade-out times are adjustable live ('+'/'-' and '>'/'<' respectively
// while in effects mode); 0 means snap instantly. The chase's fadeMode has
// no effect here.
static unsigned long effectFadeInMs = 250;
static unsigned long effectFadeOutMs = 250;
// Both fade times adjust in fine steps: 1ms unshifted ('='/'-' for fade-in,
// '.'/',' for fade-out), 10ms shifted ('+'/'_' and '>'/'<') -- the shifted
// characters are how "shift held" arrives over serial.
static const long EFFECT_FADE_FINE_STEP_MS = 1;
static const long EFFECT_FADE_COARSE_STEP_MS = 10;
static const unsigned long EFFECT_FADE_MAX_MS = 2000;

struct EffectFade {
  uint16_t startDuty;   // duty when the current ramp began
  uint16_t targetDuty;  // duty the latest frame asked for
  uint16_t currentDuty; // the ramp's current position (before noise)
  unsigned long startMillis;
  unsigned long durationMs;
};
static EffectFade effectFades[EFFECT_CHANNELS];

// Time-evolving noise for effects mode: every channel follows its own
// wandering random value -- a fresh sample every noisePeriodMs, smoothstep-
// interpolated in between, so it drifts rather than jumps. Three depths say
// how strongly that noise randomizes the effect, each individually 0-100%
// (0 = off, the default):
//   noiseBrightnessPct - lit channels are continuously dimmed by up to this
//                        share of their level (a live shimmer)
//   noiseFadeInPct     - each upward ramp's duration is scaled 0..2x by the
//                        noise value sampled as the ramp starts
//   noiseFadeOutPct    - same, for downward ramps
// 'a'/'z', 's'/'x', 'd'/'c' raise/lower those in 10% steps; 'f'/'v' slows
// down / speeds up the wandering itself (noisePeriodMs).
static uint8_t noiseBrightnessPct = 0;
static uint8_t noiseFadeInPct = 0;
static uint8_t noiseFadeOutPct = 0;
static const uint8_t NOISE_PCT_STEP = 10;
static unsigned long noisePeriodMs = 500;
static const unsigned long NOISE_PERIOD_STEP_MS = 100;
static const unsigned long NOISE_PERIOD_MIN_MS = 100;
static const unsigned long NOISE_PERIOD_MAX_MS = 5000;
static uint8_t noisePrev[EFFECT_CHANNELS];
static uint8_t noiseNext[EFFECT_CHANNELS];
static unsigned long noiseSegment = 0;

// Rolls the noise streams forward when a period boundary passes. If more
// than one period went by (or noisePeriodMs just changed), resample both
// ends instead of promoting a stale sample.
void updateEffectNoise() {
  unsigned long seg = millis() / noisePeriodMs;
  if (seg == noiseSegment) {
    return;
  }
  bool jumped = (seg != noiseSegment + 1);
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    noisePrev[i] = jumped ? (uint8_t)random(256) : noiseNext[i];
    noiseNext[i] = (uint8_t)random(256);
  }
  noiseSegment = seg;
}

// The channel's noise value right now, 0-255.
uint8_t effectNoise(uint8_t channel) {
  uint32_t frac = (millis() % noisePeriodMs) * 255 / noisePeriodMs; // 0-255
  uint32_t s = frac * frac * (765 - 2 * frac) / 65025;              // smoothstep, 0-255
  int32_t delta = (int32_t)noiseNext[channel] - (int32_t)noisePrev[channel];
  return (uint8_t)((int32_t)noisePrev[channel] + delta * (int32_t)s / 255);
}

void adjustNoisePct(uint8_t *value, int delta, const char *label) {
  int updated = (int)*value + delta;
  if (updated < 0) {
    updated = 0;
  } else if (updated > 100) {
    updated = 100;
  }
  *value = (uint8_t)updated;
  Serial.printf("%s = %d%%\n", label, updated);
}

void adjustNoisePeriod(long deltaMs) {
  long updated = (long)noisePeriodMs + deltaMs;
  if (updated < (long)NOISE_PERIOD_MIN_MS) {
    updated = NOISE_PERIOD_MIN_MS;
  } else if (updated > (long)NOISE_PERIOD_MAX_MS) {
    updated = NOISE_PERIOD_MAX_MS;
  }
  noisePeriodMs = (unsigned long)updated;
  Serial.printf("NOISE_PERIOD_MS = %lu\n", noisePeriodMs);
}

// The extender's channels can't be PWM-addressed by the original link
// frames (per-channel on/off or all-channels-same brightness only), so
// fades stream over the [0xAC][channel][duty] frame instead -- throttled
// and only-on-change, since a full 3-channel update is 9 bytes (~9ms of
// blocking SoftwareSerial TX at 9600 baud) and fades tick every ~10ms.
// The extender must run firmware that understands 0xAC frames.
static const unsigned long LINK_DUTY_INTERVAL_MS = 30;
static unsigned long lastLinkDutyMillis = 0;
static uint8_t lastSentExtenderDuty[3] = {0, 0, 0};

// Last level actually written to each local pin (after noise), so the
// shimmer only costs an analogWrite when the value really changed.
static uint16_t lastLocalOut[4] = {0, 0, 0, 0};

void sendLinkChannelDuty(uint8_t channel, uint8_t duty8) {
  linkSerial.write(LINK_DUTY_SYNC_BYTE);
  linkSerial.write(channel);
  linkSerial.write(duty8);
}

// Zeroes the engine so its notion of "current" matches channels that were
// just forced dark (see the 'e' handler, which blacks out before this),
// and reseeds the noise streams so a re-entry starts from fresh samples.
void resetEffectFades() {
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    effectFades[i].startDuty = 0;
    effectFades[i].targetDuty = 0;
    effectFades[i].currentDuty = 0;
    effectFades[i].startMillis = 0;
    effectFades[i].durationMs = 0;
    noisePrev[i] = (uint8_t)random(256);
    noiseNext[i] = (uint8_t)random(256);
  }
  noiseSegment = millis() / noisePeriodMs;
  for (uint8_t i = 0; i < 3; i++) {
    lastSentExtenderDuty[i] = 0;
  }
  for (uint8_t i = 0; i < 4; i++) {
    lastLocalOut[i] = 0;
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
  bool rampsUp = target > f.currentDuty;
  unsigned long duration = rampsUp ? effectFadeInMs : effectFadeOutMs;
  uint8_t pct = rampsUp ? noiseFadeInPct : noiseFadeOutPct;
  if (pct > 0) {
    // scale this ramp's duration 0..2x (centered on 1x) by the channel's
    // noise value at the moment the ramp starts
    int32_t bipolar = (int32_t)effectNoise(channel) - 128; // -128..127
    duration = (unsigned long)((int32_t)duration * (12800 + bipolar * (int32_t)pct) / 12800);
  }
  f.durationMs = duration;
}

void updateEffectFades() {
  if (!effectsMode) {
    return;
  }
  updateEffectNoise();
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
    // currentDuty stays the un-noised ramp position (it advances even when
    // the extender send is throttled), so retarget baselines and the
    // brightness shimmer never feed back into each other.
    f.currentDuty = duty;
    uint16_t out = duty;
    if (noiseBrightnessPct > 0 && out > 0) {
      uint32_t nv = effectNoise(i);
      out = (uint16_t)((uint32_t)out * (25500 - nv * noiseBrightnessPct) / 25500);
    }
    if (i < 4) {
      if (out != lastLocalOut[i]) {
        analogWrite(EFFECT_LOCAL_PINS[i], activeLow ? PWM_MAX - out : out);
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

void adjustEffectFadeMs(unsigned long *value, long deltaMs, const char *label) {
  long updated = (long)*value + deltaMs;
  if (updated < 0) {
    updated = 0;
  } else if (updated > (long)EFFECT_FADE_MAX_MS) {
    updated = EFFECT_FADE_MAX_MS;
  }
  *value = (unsigned long)updated;
  Serial.printf("%s = %lu\n", label, *value);
}

void applyEffectFrame() {
  const Effect &fx = EFFECTS[effectIndex];
  uint16_t duty[EFFECT_CHANNELS];
  fx.frame(EFFECT_CHANNELS, 1, effectStep, duty);

  // duty[] is indexed by physical position along the strip; each channel
  // picks up the duty of the position its lamp actually sits at.
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    setEffectTarget(i, duty[channelPosition[i]]);
  }
  updateEffectFades(); // start the ramps (or snap, at 0ms) right away

  char pattern[EFFECT_CHANNELS + 1];
  for (uint8_t i = 0; i < EFFECT_CHANNELS; i++) {
    pattern[i] = duty[i] > 0 ? 'X' : '.';
  }
  pattern[EFFECT_CHANNELS] = '\0';
  Serial.printf("[FX %s] step %u/%u %s\n", fx.name, (unsigned)(effectStep + 1),
                (unsigned)fx.stepCount(EFFECT_CHANNELS, 1), pattern);
}

void selectEffect(uint8_t index) {
  effectIndex = index;
  effectStep = 0;
  Serial.printf("[FX] effect %u/%u: %s\n", (unsigned)(effectIndex + 1),
                (unsigned)EFFECT_COUNT, EFFECTS[effectIndex].name);
  applyEffectFrame();
}

// Chase sequence: exactly one lamp lit at a time, chaseStepMs per step,
// walking physical positions 0-6 in order. Which pin lights on each step
// comes from the light-order map (see channelPosition above), so the pulse
// always travels down the physical row even when the lamps aren't hung in
// pin order. chaseStepMs is adjustable live over serial with ']'/'[' --
// since loop() re-reads it fresh at the start of every step, a change
// lands on the next step rather than mid-step.
static const uint8_t CHASE_STEP_COUNT = EFFECT_CHANNELS;
static unsigned long chaseStepMs = 500;
static const unsigned long CHASE_STEP_STEP_MS = 50;
static const unsigned long CHASE_STEP_MIN_MS = 1;
static const unsigned long CHASE_STEP_MAX_MS = 1000;

void adjustChaseStep(long deltaMs) {
  long updated = (long)chaseStepMs + deltaMs;
  if (updated < (long)CHASE_STEP_MIN_MS) {
    updated = CHASE_STEP_MIN_MS;
  } else if (updated > (long)CHASE_STEP_MAX_MS) {
    updated = CHASE_STEP_MAX_MS;
  }
  chaseStepMs = (unsigned long)updated;
  Serial.printf("CHASE_STEP_MS = %lu\n", chaseStepMs);
}

void applyChaseStep(uint8_t step) {
  for (uint8_t ch = 0; ch < EFFECT_CHANNELS; ch++) {
    bool on = channelPosition[ch] == step;
    if (ch < 4) {
      applyLocalPin(EFFECT_LOCAL_PINS[ch], on);
    } else {
      sendLinkCommand(ch - 4, on, fadeMode);
    }
  }

  Serial.printf("chase position %u/%u -> %s%s\n", (unsigned)(step + 1),
                (unsigned)CHASE_STEP_COUNT, CHANNEL_NAMES[channelAtPosition(step)],
                fadeMode ? " (fade)" : "");
}

// Send '1' for active-LOW, '0' for active-HIGH, '2' to toggle the PWM fade,
// '+'/'-' to adjust the fade duration (or the shared brightness while in
// test-all mode; '=' also counts as '+' for keyboards where '+' needs
// shift), ']'/'[' to adjust the chase step length, 'b' to toggle blackout,
// 't' to toggle test-all mode. Line endings and whitespace are ignored;
// any other unrecognized byte is echoed back with its hex code so a
// terminal sending something unexpected is visible instead of silent.
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (calibrateMode) {
      handleCalibrateKey(c); // swallows every key so digits stay calibration input
      continue;
    }
    if (c == '1') {
      if (!activeLow) {
        activeLow = true;
        Serial.println("ACTIVE_LOW = true");
        if (testAllMode) {
          applyTestAll(); // re-drive at the new polarity right away
        }
      }
    } else if (c == '0') {
      if (activeLow) {
        activeLow = false;
        Serial.println("ACTIVE_LOW = false");
        if (testAllMode) {
          applyTestAll();
        }
      }
    } else if (c == '2') {
      fadeMode = !fadeMode;
      Serial.printf("FADE_MODE = %s\n", fadeMode ? "true" : "false");
    } else if (c == '+' || c == '=') {
      if (testAllMode) {
        adjustTestBrightness(TEST_BRIGHTNESS_STEP);
      } else if (effectsMode) {
        adjustEffectFadeMs(&effectFadeInMs,
                           (c == '+') ? EFFECT_FADE_COARSE_STEP_MS : EFFECT_FADE_FINE_STEP_MS,
                           "EFFECT_FADE_IN_MS");
      } else {
        adjustFadeDuration((long)FADE_DURATION_STEP_MS);
      }
    } else if (c == '-' || c == '_') {
      if (testAllMode) {
        adjustTestBrightness(-TEST_BRIGHTNESS_STEP);
      } else if (effectsMode) {
        adjustEffectFadeMs(&effectFadeInMs,
                           (c == '_') ? -EFFECT_FADE_COARSE_STEP_MS : -EFFECT_FADE_FINE_STEP_MS,
                           "EFFECT_FADE_IN_MS");
      } else {
        adjustFadeDuration(-(long)FADE_DURATION_STEP_MS);
      }
    } else if (c == '.') {
      adjustEffectFadeMs(&effectFadeOutMs, EFFECT_FADE_FINE_STEP_MS, "EFFECT_FADE_OUT_MS");
    } else if (c == ',') {
      adjustEffectFadeMs(&effectFadeOutMs, -EFFECT_FADE_FINE_STEP_MS, "EFFECT_FADE_OUT_MS");
    } else if (c == '>') {
      adjustEffectFadeMs(&effectFadeOutMs, EFFECT_FADE_COARSE_STEP_MS, "EFFECT_FADE_OUT_MS");
    } else if (c == '<') {
      adjustEffectFadeMs(&effectFadeOutMs, -EFFECT_FADE_COARSE_STEP_MS, "EFFECT_FADE_OUT_MS");
    } else if (c == 'a') {
      adjustNoisePct(&noiseBrightnessPct, NOISE_PCT_STEP, "NOISE_BRIGHTNESS_PCT");
    } else if (c == 'z') {
      adjustNoisePct(&noiseBrightnessPct, -NOISE_PCT_STEP, "NOISE_BRIGHTNESS_PCT");
    } else if (c == 's') {
      adjustNoisePct(&noiseFadeInPct, NOISE_PCT_STEP, "NOISE_FADE_IN_PCT");
    } else if (c == 'x') {
      adjustNoisePct(&noiseFadeInPct, -NOISE_PCT_STEP, "NOISE_FADE_IN_PCT");
    } else if (c == 'd') {
      adjustNoisePct(&noiseFadeOutPct, NOISE_PCT_STEP, "NOISE_FADE_OUT_PCT");
    } else if (c == 'c') {
      adjustNoisePct(&noiseFadeOutPct, -NOISE_PCT_STEP, "NOISE_FADE_OUT_PCT");
    } else if (c == 'f') {
      adjustNoisePeriod((long)NOISE_PERIOD_STEP_MS);
    } else if (c == 'v') {
      adjustNoisePeriod(-(long)NOISE_PERIOD_STEP_MS);
    } else if (c == ']') {
      adjustChaseStep((long)CHASE_STEP_STEP_MS);
    } else if (c == '[') {
      adjustChaseStep(-(long)CHASE_STEP_STEP_MS);
    } else if (c == 'b') {
      if (testAllMode) {
        Serial.println("(blackout ignored while TEST_ALL active -- press 't' to exit first)");
      } else if (effectsMode) {
        Serial.println("(blackout ignored while EFFECTS active -- press 'e' to exit first)");
      } else {
        blackout = !blackout;
        Serial.printf("BLACKOUT = %s\n", blackout ? "true" : "false");
        if (blackout) {
          applyBlackout();
        }
      }
    } else if (c == 't') {
      testAllMode = !testAllMode;
      if (testAllMode) {
        blackout = false; // test-all overrides an active blackout or effects mode
        effectsMode = false;
        testBrightness = TEST_BRIGHTNESS_INITIAL;
        Serial.printf("TEST_ALL = true (all channels on, brightness %d/%d, '+'/'-' adjusts)\n",
                      testBrightness, PWM_MAX);
        applyTestAll();
      } else {
        Serial.println("TEST_ALL = false");
        applyBlackout(); // everything off; the chase re-lights on its next step
      }
    } else if (c == 'e') {
      effectsMode = !effectsMode;
      if (effectsMode) {
        blackout = false; // effects mode overrides an active blackout or test-all
        testAllMode = false;
        applyBlackout();    // known-dark baseline (also cancels any chase fade)...
        resetEffectFades(); // ...so the engine's zeroed state matches the pins
        Serial.println("EFFECTS_MODE = true ('n'/'p' effect, ']'/'[' speed, '='/'-' fade-in, '.'/',' fade-out (x10 shifted), 'e' exits;");
        Serial.printf("noise depths: 'a'/'z' brightness %u%%, 's'/'x' fade-in %u%%, 'd'/'c' fade-out %u%%, 'f'/'v' period %lums)\n",
                      (unsigned)noiseBrightnessPct, (unsigned)noiseFadeInPct, (unsigned)noiseFadeOutPct, noisePeriodMs);
        selectEffect(effectIndex);
      } else {
        Serial.println("EFFECTS_MODE = false");
        applyBlackout(); // everything off; the chase re-lights on its next step
      }
    } else if (c == 'm') {
      blackout = false; // calibration overrides blackout/test-all/effects
      testAllMode = false;
      effectsMode = false;
      beginCalibrate();
    } else if (c == 'n' || c == 'p') {
      if (!effectsMode) {
        Serial.println("('n'/'p' only selects effects while EFFECTS active -- press 'e' first)");
      } else if (c == 'n') {
        selectEffect((effectIndex + 1) % EFFECT_COUNT);
      } else {
        selectEffect((effectIndex + EFFECT_COUNT - 1) % EFFECT_COUNT);
      }
    } else if (c != '\r' && c != '\n' && c != '\t' && c != ' ' && c != 0) {
      Serial.printf("(unhandled key 0x%02X '%c')\n", (uint8_t)c, (c >= 32 && c < 127) ? c : '?');
    }
  }
}

// Same as delay(), but keeps polling serial, the link status, and any
// in-progress fade so they all stay responsive within ~10ms instead of
// waiting out the rest of the chase step.
void delayWithSerial(unsigned long ms) {
  unsigned long start = millis();
  do {
    handleSerial();
    updateLinkStatus();
    updateFade();
    updateEffectFades();
    delay(10);
  } while (millis() - start < ms);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nMULTIPLEX_8266 starting...");
  Serial.println("Send '1' for active-LOW, '0' for active-HIGH (default), '2' to toggle PWM fade,");
  Serial.println("'+'/'-' to adjust fade duration, ']'/'[' to adjust chase step length, 'b' to toggle blackout,");
  Serial.println("'t' to toggle test-all mode (all channels on; '+'/'-' then adjusts brightness),");
  Serial.println("'e' to toggle effects test mode ('n'/'p' selects the effect; '='/'-' adjusts its");
  Serial.println("fade-in and '.'/',' its fade-out, 1ms per press, or 10ms shifted ('+'/'_' and");
  Serial.println("'>'/'<'), 0 = instant).");
  Serial.println("Effects noise depths: 'a'/'z' brightness, 's'/'x' fade-in, 'd'/'c' fade-out");
  Serial.println("(10% per press, 0 = off), 'f'/'v' noise period (100ms per press).");
  Serial.println("'m' to calibrate the physical light order (each channel lights alone; type its");
  Serial.println("position 1-7; the order is saved to EEPROM and survives reboots).");

  // Seed the effects noise from the hardware RNG so each boot wanders
  // differently -- Arduino random() is otherwise deterministic.
  randomSeed(RANDOM_REG32);

  bootFlashStatusLed();
  analogWriteRange(PWM_MAX);

  EEPROM.begin(MAP_EEPROM_SIZE);
  loadChannelMap();

  linkSerial.begin(LINK_BAUD);
  linkStateSinceMillis = millis();

  pinMode(MOSFET_PIN_A, OUTPUT);
  pinMode(MOSFET_PIN_B, OUTPUT);
  pinMode(MOSFET_PIN_C, OUTPUT);
  pinMode(MOSFET_PIN_D, OUTPUT);
}

void loop() {
  if (effectsMode) {
    // selectEffect() already applied the current frame; hold it for one
    // step, then advance. Re-check the mode after the delay -- 'e', 't',
    // or a switched effect may have landed mid-delay via handleSerial().
    delayWithSerial(chaseStepMs);
    if (effectsMode) {
      effectStep = (effectStep + 1) % EFFECTS[effectIndex].stepCount(EFFECT_CHANNELS, 1);
      applyEffectFrame();
    }
    return;
  }
  if (testAllMode || blackout || calibrateMode) {
    delayWithSerial(50); // idle, but still responsive to 't'/'b'/'m' and '+'/'-'
    return;
  }
  for (uint8_t step = 0; step < CHASE_STEP_COUNT; step++) {
    if (blackout || testAllMode || effectsMode || calibrateMode) {
      break; // bail out mid-sequence so a mode change doesn't wait out the chase
    }
    applyChaseStep(step);
    delayWithSerial(chaseStepMs);
  }
}
