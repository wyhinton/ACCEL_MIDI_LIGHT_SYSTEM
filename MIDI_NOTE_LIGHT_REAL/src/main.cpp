/*
  MIDI Note Recorder Bridge – ESP32-S3 / Seeed XIAO ESP32-S3
  (branch: midi_rec_for_recording)

  On this branch the board does NOTHING with lights, the relay, or ESP-NOW.
  It is a dumb, fast MIDI <-> USB bridge for the companion tkinter recorder UI
  (MIDI_RECORDER_UI/midi_recorder.py):

    HARDWARE MIDI IN  (FeatherWing, Serial1) ── parsed ──▶ USB Serial lines ──▶ PC
    PC ──▶ USB Serial commands ── emitted ──▶ HARDWARE MIDI OUT (FeatherWing, Serial1)

  The PC side does the recording (host clock) and timed playback; the firmware
  just translates between the 31250-baud DIN MIDI world and a simple, line-based
  ASCII protocol over the 115200-baud USB serial link. Keeping it line-based
  means you can also just open the PlatformIO serial monitor and read/debug it
  by hand.

  ---- Lines the board SENDS to the PC (newline-terminated) -----------------
    READY <fwName> <version>            once, at the end of setup()
    #<text>                             human-readable info / banner (ignored by parser)
    EVT <ms> <TYPE> <ch> <d1> <d2>      one MIDI event. TYPE in:
                                          NON NOF CC PB PC AT CAT  (see typeName())
                                        ms = board millis() when received.
    STAT <ms> in=<n> out=<n> err=<n>    periodic heartbeat / counters
    PONG <ms>                           reply to a PING
    ECHO <text>                         reply to an ECHO command (link test)

  ---- Lines the board ACCEPTS from the PC (for playback / testing) ----------
    PING                                -> PONG
    ECHO <text>                         -> ECHO <text>
    STAT                                -> emit a STAT line now
    PNON <ch> <note> <vel>              send Note On out the MIDI OUT jack
    PNOF <ch> <note> <vel>              send Note Off out the MIDI OUT jack
    PCC  <ch> <num> <val>               send Control Change
    PPB  <ch> <value14>                 send Pitch Bend (0..16383, 8192=center)
    PPC  <ch> <prog>                    send Program Change
    ALLOFF [ch]                         All Notes Off (CC123) on ch, or all chans
    RAW <hexbyte> [hexbyte ...]         emit arbitrary raw bytes out MIDI OUT

  Channels in the protocol are 1..16 (human MIDI numbering).
*/

#include <Arduino.h>
#include <MIDI.h>

// -------- IDENTITY --------
#define FW_NAME    "midi-recorder-bridge"
#define FW_VERSION "1.0"

// -------- HARDWARE MIDI (FeatherWing on Serial1) --------
// XIAO ESP32-S3 silkscreened UART pins:
//   D7 = GPIO44 = RX  (wire to the wing's TX / MIDI IN)
//   D6 = GPIO43 = TX  (wire to the wing's RX / MIDI OUT)
#define MIDI_RX_PIN 44
#define MIDI_TX_PIN 43

HardwareSerial MidiSerial(1);
MIDI_CREATE_INSTANCE(HardwareSerial, MidiSerial, MIDI);

// -------- COUNTERS (exposed in STAT) --------
unsigned long inCount  = 0;   // MIDI events received from the wire and forwarded
unsigned long outCount = 0;   // MIDI events emitted from PC commands
unsigned long errCount = 0;   // malformed PC commands

// Short token for each MIDI message type. Keep in sync with the parser in
// midi_recorder.py.
const char *typeName(midi::MidiType t) {
  switch (t) {
    case midi::NoteOn:               return "NON";
    case midi::NoteOff:              return "NOF";
    case midi::ControlChange:        return "CC";
    case midi::PitchBend:            return "PB";
    case midi::ProgramChange:        return "PC";
    case midi::AfterTouchPoly:       return "AT";   // d1=note d2=pressure
    case midi::AfterTouchChannel:    return "CAT";  // d1=pressure
    default:                         return "OTH";
  }
}

// Forward one parsed message to the PC as an EVT line. For pitch bend we report
// the 14-bit value in d1 (d2 left 0) so the PC doesn't have to re-stitch bytes.
void emitEvent(const midi::Message<128> &msg) {
  // Skip the high-rate System Real-Time stream (clock / active sensing) so the
  // recorder isn't flooded; it carries no note content.
  if (msg.type == midi::Clock || msg.type == midi::ActiveSensing ||
      msg.type == midi::Start  || msg.type == midi::Continue     ||
      msg.type == midi::Stop) {
    return;
  }

  int d1 = msg.data1;
  int d2 = msg.data2;
  if (msg.type == midi::PitchBend) {
    // MIDI.h delivers pitch bend already combined in data1/data2 (LSB/MSB).
    d1 = ((msg.data2 << 7) | msg.data1);  // 0..16383
    d2 = 0;
  }

  Serial.print("EVT ");
  Serial.print(millis());      Serial.print(' ');
  Serial.print(typeName(msg.type)); Serial.print(' ');
  Serial.print(msg.channel);   Serial.print(' ');
  Serial.print(d1);            Serial.print(' ');
  Serial.println(d2);
  inCount++;
}

// -------- STAT heartbeat --------
#define STAT_INTERVAL_MS 2000
unsigned long lastStatMs = 0;

void emitStat() {
  Serial.print("STAT ");
  Serial.print(millis());
  Serial.print(" in=");  Serial.print(inCount);
  Serial.print(" out="); Serial.print(outCount);
  Serial.print(" err="); Serial.println(errCount);
}

// =====================================================================
//  PC command parser (lines arriving on USB Serial)
// =====================================================================
char    cmdBuf[96];
uint8_t cmdLen = 0;

// Parse up to 4 integer args after the command token. Returns how many parsed.
int parseArgs(char *p, long *out, int maxArgs) {
  int n = 0;
  while (n < maxArgs) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    char *end = nullptr;
    long v = strtol(p, &end, 0);   // base 0 -> also accepts 0x.. for RAW
    if (end == p) break;           // no number consumed
    out[n++] = v;
    p = end;
  }
  return n;
}

void sendAllOff(int ch) {
  // CC 123 = All Notes Off.
  MIDI.sendControlChange(123, 0, ch);
  outCount++;
}

void handleCommand(char *line) {
  // Split the leading command token from the rest.
  char *args = line;
  while (*args && *args != ' ' && *args != '\t') args++;
  char saved = *args;
  *args = '\0';                 // terminate the token
  const char *cmd = line;
  char *rest = (saved == '\0') ? args : args + 1;

  long a[4];

  if (strcasecmp(cmd, "PING") == 0) {
    Serial.print("PONG "); Serial.println(millis());

  } else if (strcasecmp(cmd, "ECHO") == 0) {
    Serial.print("ECHO "); Serial.println(rest);

  } else if (strcasecmp(cmd, "STAT") == 0) {
    emitStat();

  } else if (strcasecmp(cmd, "PNON") == 0) {
    if (parseArgs(rest, a, 3) == 3) { MIDI.sendNoteOn(a[1], a[2], a[0]); outCount++; }
    else errCount++;

  } else if (strcasecmp(cmd, "PNOF") == 0) {
    if (parseArgs(rest, a, 3) == 3) { MIDI.sendNoteOff(a[1], a[2], a[0]); outCount++; }
    else errCount++;

  } else if (strcasecmp(cmd, "PCC") == 0) {
    if (parseArgs(rest, a, 3) == 3) { MIDI.sendControlChange(a[1], a[2], a[0]); outCount++; }
    else errCount++;

  } else if (strcasecmp(cmd, "PPB") == 0) {
    // value 0..16383 (8192 center) -> MIDI.h wants -8192..8191
    if (parseArgs(rest, a, 2) == 2) { MIDI.sendPitchBend((int)(a[1] - 8192), a[0]); outCount++; }
    else errCount++;

  } else if (strcasecmp(cmd, "PPC") == 0) {
    if (parseArgs(rest, a, 2) == 2) { MIDI.sendProgramChange(a[1], a[0]); outCount++; }
    else errCount++;

  } else if (strcasecmp(cmd, "ALLOFF") == 0) {
    int n = parseArgs(rest, a, 1);
    if (n == 1) sendAllOff(a[0]);
    else for (int ch = 1; ch <= 16; ch++) sendAllOff(ch);

  } else if (strcasecmp(cmd, "RAW") == 0) {
    long bytes[16];
    int n = parseArgs(rest, bytes, 16);
    for (int i = 0; i < n; i++) MidiSerial.write((uint8_t)bytes[i]);
    if (n > 0) outCount++; else errCount++;

  } else if (cmd[0] == '\0') {
    // blank line, ignore

  } else {
    errCount++;
    Serial.print("# unknown cmd: "); Serial.println(cmd);
  }
}

void pumpSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[cmdLen] = '\0';
      handleCommand(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    } else {
      // overflow: reset the line so we don't act on a truncated command
      cmdLen = 0;
      errCount++;
    }
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Hardware DIN MIDI: 31250 baud, 8N1, on the FeatherWing UART pins.
  MidiSerial.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);

  // Receive on every channel and hand each message to emitEvent(). Turn OFF
  // the library's automatic Thru so incoming MIDI isn't blindly echoed back out
  // (the PC controls MIDI OUT explicitly during playback).
  MIDI.setHandleMessage(emitEvent);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  Serial.println();
  Serial.print("# "); Serial.print(FW_NAME); Serial.print(' '); Serial.println(FW_VERSION);
  Serial.print("# MIDI IN/OUT on Serial1 RX=GPIO"); Serial.print(MIDI_RX_PIN);
  Serial.print(" TX=GPIO"); Serial.print(MIDI_TX_PIN); Serial.println(" @ 31250 8N1");
  Serial.println("# USB link @ 115200. Send PING for PONG. Recorder UI: MIDI_RECORDER_UI/midi_recorder.py");
  Serial.print("READY "); Serial.print(FW_NAME); Serial.print(' '); Serial.println(FW_VERSION);
}

void loop() {
  MIDI.read();            // parse hardware MIDI -> emitEvent() -> EVT lines
  pumpSerialCommands();   // PC commands -> MIDI OUT / replies

  unsigned long now = millis();
  if (now - lastStatMs >= STAT_INTERVAL_MS) {
    lastStatMs = now;
    emitStat();
  }
}
