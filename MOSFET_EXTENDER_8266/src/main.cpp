// Pin map (extender board):
//   GPIO0  (D3) - MOSFET channel 2 (boot strapping pin, see MOSFET_PIN_C)
//   GPIO2  (D4) - onboard status LED, active-LOW (see STATUS_LED_PIN)
//   GPIO4  (D2) - MOSFET channel 0
//   GPIO5  (D1) - MOSFET channel 1
//   GPIO12 (D6) - link RX from primary board
//   GPIO14 (D5) - link TX to primary board

#include <Arduino.h>
#include <SoftwareSerial.h>

// GPIO extender board: takes commands over UART from the primary
// (MULTIPLEX_8266) and drives three more MOSFET switch modules directly.
// GPIO0 is a boot strapping pin (must read HIGH at power-on for normal
// boot), but that only matters during reset -- driving it after setup()
// is safe.
static const uint8_t MOSFET_PIN_A = 4; // channel 0
static const uint8_t MOSFET_PIN_B = 5; // channel 1
static const uint8_t MOSFET_PIN_C = 0; // channel 2 (D3)

// Link to the primary board. Runs on SoftwareSerial (not the hardware UART)
// so USB debug output and the '1'/'0' active-low toggle below keep working
// over the USB cable. GPIO12/14 (D6/D5) are safe pins with no boot
// strapping behavior. Wire crossed: primary TX (D5) -> this RX (D6),
// primary RX (D6) -> this TX (D5), plus a shared GND between the two boards.
static const uint8_t LINK_RX_PIN = 12; // D6
static const uint8_t LINK_TX_PIN = 14; // D5
static const uint32_t LINK_BAUD = 9600;
SoftwareSerial linkSerial(LINK_RX_PIN, LINK_TX_PIN);

// Frames from the primary: [0xAA sync][cmd] for per-channel on/off -- cmd
// bit0 = state (1=on, 0=off), bits1-2 = channel (0-2), bit3 = fade mode
// (see applyChannel()) -- [0xAB sync][brightness 0-255] to drive every
// channel to the same raw duty (the primary's test-all mode, see
// handleAllBrightness()) -- or [0xAC sync][channel 0-2][duty 0-255] to set
// one channel's raw duty (streamed by the primary's effects fade engine,
// see handleChannelDuty()). The sync byte lets us resync after
// noise/garbage instead of misreading a stray byte as a command.
static const uint8_t LINK_SYNC_BYTE = 0xAA;
static const uint8_t LINK_BRIGHTNESS_SYNC_BYTE = 0xAB;
static const uint8_t LINK_DUTY_SYNC_BYTE = 0xAC;
static uint8_t pendingSync = 0;         // 0 = waiting for a sync byte
static uint8_t pendingDutyChannel = 0xFF; // 0xAC frames: 0xFF = channel byte not yet received

// Sent back to the primary on its own schedule (independent of command
// traffic) so its logs can tell whether this board is actually connected,
// not just assume it. Content doesn't matter -- the primary only cares that
// bytes keep arriving.
static const unsigned long HEARTBEAT_INTERVAL_MS = 250;
static unsigned long lastHeartbeatMillis = 0;

void sendHeartbeatIfDue() {
  unsigned long now = millis();
  if (now - lastHeartbeatMillis >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMillis = now;
    linkSerial.write(LINK_SYNC_BYTE);
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

// Mirrors the primary's fade behavior for a channel commanded "on" with the
// fade bit set: ramp 0 -> PWM_MAX over FADE_DURATION_MS instead of snapping
// straight to full. Doesn't need to match the primary's timing exactly --
// it's just driving a light -- but is kept the same for consistency.
static const unsigned long FADE_DURATION_MS = 250;
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
  if (elapsed >= FADE_DURATION_MS) {
    analogWrite(fadingPin, activeLow ? 0 : PWM_MAX);
    fadingPin = 255;
    return;
  }
  int duty = (int)((uint32_t)elapsed * PWM_MAX / FADE_DURATION_MS);
  analogWrite(fadingPin, activeLow ? (PWM_MAX - duty) : duty);
}

static bool mosfetAOn = false;
static bool mosfetBOn = false;
static bool mosfetCOn = false;

void applyChannel(uint8_t pin, bool on, bool fade) {
  if (on && fade) {
    beginFade(pin);
  } else if (on) {
    setMosfet(pin, true);
  } else {
    cancelFade(pin);
    setMosfet(pin, false);
  }
  if (pin == MOSFET_PIN_A) {
    mosfetAOn = on;
  } else if (pin == MOSFET_PIN_B) {
    mosfetBOn = on;
  } else {
    mosfetCOn = on;
  }
}

void logMosfetState() {
  Serial.printf("GPIO4=%s GPIO5=%s GPIO0=%s\n",
                mosfetAOn ? "ON " : "off",
                mosfetBOn ? "ON " : "off",
                mosfetCOn ? "ON " : "off");
}

// Per-channel raw duty from the primary's effects fades. The primary
// streams these continuously while a channel ramps, so unlike the other
// frame handlers this one doesn't log -- per-frame prints would flood the
// console. A duty write overrides (and cancels) any fade in progress.
void handleChannelDuty(uint8_t channel, uint8_t raw) {
  uint8_t pin = (channel == 0) ? MOSFET_PIN_A : (channel == 1) ? MOSFET_PIN_B : MOSFET_PIN_C;
  cancelFade(pin);
  int duty = (int)raw * PWM_MAX / 255;
  analogWrite(pin, activeLow ? PWM_MAX - duty : duty);
  bool on = duty > 0;
  if (channel == 0) {
    mosfetAOn = on;
  } else if (channel == 1) {
    mosfetBOn = on;
  } else {
    mosfetCOn = on;
  }
}

void handleLinkCommand(uint8_t cmd) {
  uint8_t channel = (cmd >> 1) & 0x3;
  bool on = cmd & 0x1;
  bool fade = (cmd >> 3) & 0x1;
  if (channel > 2) {
    return; // not a channel we have -- likely line noise, drop it
  }
  uint8_t pin = (channel == 0) ? MOSFET_PIN_A : (channel == 1) ? MOSFET_PIN_B : MOSFET_PIN_C;
  applyChannel(pin, on, fade);
  logMosfetState();
}

// Primary's test-all mode: every channel driven to the same raw duty at
// once, bypassing the per-channel on/off handling. Normal 0xAA channel
// commands simply overwrite this once the primary's chase resumes.
void handleAllBrightness(uint8_t raw) {
  fadingPin = 255; // direct duty writes below; don't let a stale fade fight them
  int duty = (int)raw * PWM_MAX / 255;
  int level = activeLow ? PWM_MAX - duty : duty;
  analogWrite(MOSFET_PIN_A, level);
  analogWrite(MOSFET_PIN_B, level);
  analogWrite(MOSFET_PIN_C, level);
  mosfetAOn = mosfetBOn = mosfetCOn = (duty > 0);
  Serial.printf("ALL brightness = %d / %d\n", duty, PWM_MAX);
}

// Reads whatever bytes are waiting on the link and applies any complete
// [sync][payload...] frames. Runs a state machine instead of blocking on
// multiple reads at once so a payload byte that hasn't arrived yet never
// stalls loop(). Which sync byte opened the frame decides how the payload
// is interpreted (and how long it is: 0xAA/0xAB carry one byte, 0xAC two).
void handleLink() {
  while (linkSerial.available()) {
    uint8_t b = linkSerial.read();
    if (pendingSync == 0) {
      if (b == LINK_SYNC_BYTE || b == LINK_BRIGHTNESS_SYNC_BYTE || b == LINK_DUTY_SYNC_BYTE) {
        pendingSync = b;
        pendingDutyChannel = 0xFF;
      }
    } else if (pendingSync == LINK_SYNC_BYTE) {
      handleLinkCommand(b);
      pendingSync = 0;
    } else if (pendingSync == LINK_BRIGHTNESS_SYNC_BYTE) {
      handleAllBrightness(b);
      pendingSync = 0;
    } else if (pendingDutyChannel == 0xFF) {
      if (b > 2) {
        pendingSync = 0; // not a channel we have -- likely line noise, drop the frame
      } else {
        pendingDutyChannel = b;
      }
    } else {
      handleChannelDuty(pendingDutyChannel, b);
      pendingSync = 0;
    }
  }
}

// Send '1' for active-LOW, '0' for active-HIGH; anything else (line endings, etc.) is ignored.
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '1' && !activeLow) {
      activeLow = true;
      Serial.println("ACTIVE_LOW = true");
    } else if (c == '0' && activeLow) {
      activeLow = false;
      Serial.println("ACTIVE_LOW = false");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nMOSFET_EXTENDER_8266 starting...");
  Serial.println("Send '1' for active-LOW, '0' for active-HIGH (default).");

  bootFlashStatusLed();
  analogWriteRange(PWM_MAX);

  linkSerial.begin(LINK_BAUD);

  pinMode(MOSFET_PIN_A, OUTPUT);
  pinMode(MOSFET_PIN_B, OUTPUT);
  pinMode(MOSFET_PIN_C, OUTPUT);
  setMosfet(MOSFET_PIN_A, false); // off until the primary says otherwise
  setMosfet(MOSFET_PIN_B, false);
  setMosfet(MOSFET_PIN_C, false);
}

void loop() {
  handleLink();
  handleSerial();
  sendHeartbeatIfDue();
  updateFade();
}
