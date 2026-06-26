/*
  MIDI Note Recorder Bridge – ESP32-S3 / Seeed XIAO ESP32-S3
  (branch: midi_rec_for_recording)

  On this branch the board does NOTHING with lights, the relay, or ESP-NOW.
  It is a dumb, fast MIDI <-> {USB, WiFi} bridge for the companion tkinter
  recorder UI (MIDI_RECORDER_UI/midi_recorder.py):

    HARDWARE MIDI IN  (FeatherWing, Serial1) ── parsed ──▶ USB + WiFi lines ──▶ PC
    PC ──▶ USB/WiFi commands ── emitted ──▶ HARDWARE MIDI OUT (FeatherWing, Serial1)

  TWO TRANSPORTS, SAME PROTOCOL:
    * USB Serial @ 115200 (always on; good for flashing/debug/fallback).
    * WiFi SoftAP + TCP server: the board hosts its own WiFi network and a PC
      connects a TCP socket to it. No router required. Lines are mirrored to
      whichever transports are connected, and commands are accepted from both.

  SoftAP defaults (change AP_SSID / AP_PASS below):
    SSID: "MIDI-Recorder"   PASS: "midi1234"   board IP: 192.168.4.1   PORT 5000

  The PC side does the recording (using each event's board timestamp, so WiFi
  jitter doesn't smear timing) and timed playback; the firmware just translates
  between the 31250-baud DIN MIDI world and a simple, line-based ASCII protocol.

  ---- Lines the board SENDS (newline-terminated) ---------------------------
    READY <fwName> <version>            once, at the end of setup()
    #<text>                             human-readable info / banner (ignored by parser)
    EVT <ms> <TYPE> <ch> <d1> <d2>      one MIDI event. TYPE in:
                                          NON NOF CC PB PC AT CAT
                                        ms = board millis() when received.
    STAT <ms> in=<n> out=<n> err=<n> net=<0|1>   periodic heartbeat / counters
    PONG <ms>                           reply to a PING
    ECHO <text>                         reply to an ECHO command (link test)

  ---- Lines the board ACCEPTS (from USB and/or the TCP client) --------------
    PING | ECHO <text> | STAT
    PNON <ch> <note> <vel>   PNOF <ch> <note> <vel>   PCC <ch> <num> <val>
    PPB <ch> <value14>       PPC <ch> <prog>          ALLOFF [ch]
    RAW <hexbyte> [hexbyte ...]
  Channels are 1..16 (human MIDI numbering).
*/

#include <Arduino.h>
#include <MIDI.h>
#include <WiFi.h>

// -------- IDENTITY --------
#define FW_NAME    "midi-recorder-bridge"
#define FW_VERSION "2.0"

// -------- WiFi SoftAP --------
// The board hosts its own network; the PC joins it (no router needed). WPA2
// requires the password to be >= 8 chars. Set AP_PASS to "" for an open AP.
#define AP_SSID  "MIDI-Recorder"
#define AP_PASS  "midi1234"
#define TCP_PORT 5000

WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;          // single active client at a time

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

// =====================================================================
//  Output: mirror a finished line to every connected transport.
// =====================================================================
// All senders build a full line (NO trailing newline) into a buffer and call
// emitLine(), which appends "\r\n" and writes it to USB Serial and, if present,
// the TCP client.
void emitLine(const char *line) {
  Serial.print(line);
  Serial.print("\r\n");
  if (tcpClient && tcpClient.connected()) {
    tcpClient.print(line);
    tcpClient.print("\r\n");
  }
}

char lineBuf[160];

// Short token for each MIDI message type. Keep in sync with midi_recorder.py.
const char *typeName(midi::MidiType t) {
  switch (t) {
    case midi::NoteOn:            return "NON";
    case midi::NoteOff:           return "NOF";
    case midi::ControlChange:     return "CC";
    case midi::PitchBend:         return "PB";
    case midi::ProgramChange:     return "PC";
    case midi::AfterTouchPoly:    return "AT";   // d1=note d2=pressure
    case midi::AfterTouchChannel: return "CAT";  // d1=pressure
    default:                      return "OTH";
  }
}

// Forward one parsed message as an EVT line.
void emitEvent(const midi::Message<128> &msg) {
  // Skip the high-rate System Real-Time stream (clock / active sensing / start/
  // stop/continue) so the recorder isn't flooded; it carries no note content.
  if (msg.type == midi::Clock || msg.type == midi::ActiveSensing ||
      msg.type == midi::Start  || msg.type == midi::Continue     ||
      msg.type == midi::Stop) {
    return;
  }

  int d1 = msg.data1;
  int d2 = msg.data2;
  if (msg.type == midi::PitchBend) {
    d1 = ((msg.data2 << 7) | msg.data1);  // combine to 0..16383
    d2 = 0;
  }

  snprintf(lineBuf, sizeof(lineBuf), "EVT %lu %s %d %d %d",
           millis(), typeName(msg.type), msg.channel, d1, d2);
  emitLine(lineBuf);
  inCount++;
}

// -------- STAT heartbeat --------
#define STAT_INTERVAL_MS 2000
unsigned long lastStatMs = 0;

void emitStat() {
  snprintf(lineBuf, sizeof(lineBuf), "STAT %lu in=%lu out=%lu err=%lu net=%d",
           millis(), inCount, outCount, errCount,
           (tcpClient && tcpClient.connected()) ? 1 : 0);
  emitLine(lineBuf);
}

// =====================================================================
//  Command parser (lines arriving on USB or TCP)
// =====================================================================
// Parse up to maxArgs integer args. base 0 also accepts 0x.. (for RAW).
int parseArgs(char *p, long *out, int maxArgs) {
  int n = 0;
  while (n < maxArgs) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    char *end = nullptr;
    long v = strtol(p, &end, 0);
    if (end == p) break;
    out[n++] = v;
    p = end;
  }
  return n;
}

void sendAllOff(int ch) {
  MIDI.sendControlChange(123, 0, ch);   // CC123 = All Notes Off
  outCount++;
}

void handleCommand(char *line) {
  char *args = line;
  while (*args && *args != ' ' && *args != '\t') args++;
  char saved = *args;
  *args = '\0';
  const char *cmd = line;
  char *rest = (saved == '\0') ? args : args + 1;

  long a[4];

  if (strcasecmp(cmd, "PING") == 0) {
    snprintf(lineBuf, sizeof(lineBuf), "PONG %lu", millis());
    emitLine(lineBuf);

  } else if (strcasecmp(cmd, "ECHO") == 0) {
    snprintf(lineBuf, sizeof(lineBuf), "ECHO %s", rest);
    emitLine(lineBuf);

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
    snprintf(lineBuf, sizeof(lineBuf), "# unknown cmd: %s", cmd);
    emitLine(lineBuf);
  }
}

// One line-assembly buffer per transport so partial lines don't interleave.
template <size_t CAP>
struct LineReader {
  char    buf[CAP];
  uint8_t len = 0;
  void feed(char c) {
    if (c == '\r') return;
    if (c == '\n') {
      buf[len] = '\0';
      handleCommand(buf);
      len = 0;
    } else if (len < CAP - 1) {
      buf[len++] = c;
    } else {
      len = 0;          // overflow -> drop the truncated line
      errCount++;
    }
  }
};

LineReader<96> usbReader;
LineReader<96> netReader;

void pumpUsb() {
  while (Serial.available()) usbReader.feed((char)Serial.read());
}

void pumpNet() {
  // Accept a new client if we don't have a live one (one at a time).
  if (!tcpClient || !tcpClient.connected()) {
    WiFiClient incoming = tcpServer.available();
    if (incoming) {
      tcpClient = incoming;
      netReader.len = 0;
      snprintf(lineBuf, sizeof(lineBuf), "# TCP client connected: %s",
               tcpClient.remoteIP().toString().c_str());
      emitLine(lineBuf);
      // greet the fresh client so its UI flips to "connected"
      snprintf(lineBuf, sizeof(lineBuf), "READY %s %s", FW_NAME, FW_VERSION);
      emitLine(lineBuf);
    }
  }
  while (tcpClient && tcpClient.connected() && tcpClient.available()) {
    netReader.feed((char)tcpClient.read());
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Hardware DIN MIDI: 31250 baud, 8N1, on the FeatherWing UART pins.
  MidiSerial.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);
  MIDI.setHandleMessage(emitEvent);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();   // PC controls MIDI OUT explicitly; don't auto-echo IN->OUT

  // WiFi SoftAP + TCP server.
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, (strlen(AP_PASS) >= 8) ? AP_PASS : nullptr);
  IPAddress ip = WiFi.softAPIP();
  tcpServer.begin();
  tcpServer.setNoDelay(true);   // low-latency: don't coalesce small MIDI packets

  Serial.println();
  Serial.print("# "); Serial.print(FW_NAME); Serial.print(' '); Serial.println(FW_VERSION);
  Serial.print("# MIDI IN/OUT on Serial1 RX=GPIO"); Serial.print(MIDI_RX_PIN);
  Serial.print(" TX=GPIO"); Serial.print(MIDI_TX_PIN); Serial.println(" @ 31250 8N1");
  if (apOk) {
    Serial.print("# WiFi AP \""); Serial.print(AP_SSID);
    Serial.print("\" pass \""); Serial.print(AP_PASS); Serial.println("\"");
    Serial.print("# Connect TCP to "); Serial.print(ip);
    Serial.print(":"); Serial.println(TCP_PORT);
  } else {
    Serial.println("# WiFi SoftAP FAILED to start");
  }
  Serial.println("# USB link @ 115200. Send PING for PONG. UI: MIDI_RECORDER_UI/midi_recorder.py");
  Serial.print("READY "); Serial.print(FW_NAME); Serial.print(' '); Serial.println(FW_VERSION);
}

void loop() {
  MIDI.read();     // parse hardware MIDI -> emitEvent() -> EVT lines (USB + TCP)
  pumpUsb();       // USB commands
  pumpNet();       // TCP accept + commands

  unsigned long now = millis();
  if (now - lastStatMs >= STAT_INTERVAL_MS) {
    lastStatMs = now;
    emitStat();
  }
}
