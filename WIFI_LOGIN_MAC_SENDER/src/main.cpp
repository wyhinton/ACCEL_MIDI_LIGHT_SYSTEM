/*
  Wi-Fi Login Bridge – MAC SENDER (ESP32-S3 / Seeed XIAO ESP32-S3)

  Plugs into the Mac over USB. A Python script on the Mac writes a line
  like:

      WIFI:<ssid>:<password>\n

  over the USB-serial port. This board parses it, packs it into a
  WifiCredMessage, and sends it over ESP-NOW to the paired receiver board
  (WIFI_LOGIN_PC_RECEIVER), which is plugged into the target Windows PC.

  The receiver forwards the credentials to a Windows-side script over ITS
  USB-serial port. That script calls `netsh wlan add profile` / `netsh
  wlan connect` directly — no keyboard/HID emulation, no UI automation.
  Once Windows reports success or failure, the receiver sends a
  WifiResultMessage back over ESP-NOW, which this board relays to the Mac
  as a `RESULT:OK` / `RESULT:FAIL:<detail>` line so the Python script can
  confirm the PC actually joined the network.

  Serial protocol (Mac <-> this board), newline-terminated ASCII:
    Mac -> board:   WIFI:<ssid>:<password>
    board -> Mac:   LINK:UP | LINK:DOWN     (ESP-NOW link to the PC board)
                    SENT                     (credentials handed to ESP-NOW)
                    RESULT:OK                (PC confirmed it connected)
                    RESULT:FAIL:<detail>     (PC reported a failure)
                    PC:<text>                (status/debug line from the PC board)
                    (any other line is just debug logging)

  Limitation: since ':' is the field separator, the SSID may not contain
  ':' (the password may — everything after the second ':' is the password).

  Status matrix (8x8 NeoMatrix on GPIO 14):
    dim red    - on, not yet linked to the PC board over ESP-NOW
    dim green  - linked (HELLO/ACK handshake with the PC board is current)
    cyan flash - credentials just handed off to ESP-NOW
    green flash- PC confirmed it joined the network
    red flash  - PC reported a failure
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

// -------- WIFI CREDENTIAL MESSAGE (this board -> PC board) --------
// MUST match WIFI_LOGIN_PC_RECEIVER's struct byte-for-byte.
#define WIFI_CRED_MAGIC 0xC1
typedef struct __attribute__((packed)) {
  uint8_t cmd;           // = WIFI_CRED_MAGIC
  char    ssid[33];      // up to 32 chars + NUL (802.11 max SSID length)
  char    password[64];  // up to 63 chars + NUL (WPA2-PSK max length)
} WifiCredMessage;

// -------- WIFI RESULT MESSAGE (PC board -> this board) --------
enum WifiResultCode { WIFI_RESULT_OK = 1, WIFI_RESULT_FAIL = 2 };
typedef struct __attribute__((packed)) {
  uint8_t cmd;        // = WIFI_RESULT_MAGIC
  uint8_t result;     // WifiResultCode
  char    detail[64]; // human-readable detail/error, NUL-terminated
} WifiResultMessage;
#define WIFI_RESULT_MAGIC 0xC2

// -------- STATUS MESSAGE (PC board -> this board) --------
// Free-form human-readable status/debug text from the PC board, relayed to
// the Mac as "PC:<text>" so its console shows what's happening on that
// board even though it's normally plugged into a headless Windows PC.
// MUST match WIFI_LOGIN_PC_RECEIVER's struct byte-for-byte.
#define STATUS_MAGIC 0xC3
typedef struct __attribute__((packed)) {
  uint8_t cmd;      // = STATUS_MAGIC
  char    text[64]; // human-readable, NUL-terminated
} StatusMessage;

// -------- HANDSHAKE / LINK STATUS --------
// This board beacons HELLO; the PC board replies ACK. Distinguished from
// the other message types by length. HS_SELFTEST_REQ asks the PC board to
// run its short HID self-test on its own (see TESTHID below) — useful for
// iterating on focus/lock-screen issues without a full credential attempt.
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2, HS_SELFTEST_REQ = 3 };
typedef struct __attribute__((packed)) {
  uint8_t type;
} HandshakeMessage;

// Broadcast: either board on the same WiFi channel will pick this up.
// To target one specific PC board instead, replace with its STA MAC
// (printed at boot) and set peer.encrypt as desired.
uint8_t espNowPeerMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

bool espNowReady = false;

const unsigned long HELLO_INTERVAL_MS = 1000;  // beacon period
const unsigned long LINK_TIMEOUT_MS   = 3000;  // no ACK this long => link down

volatile unsigned long lastAckMs = 0;  // last ACK arrival (set in recv cb)
bool          linkUp          = false; // debounced link state, printed on change
unsigned long lastHelloSentMs = 0;

// Result relayed from the PC board, queued in the recv callback and
// printed from loop() (keep callback work minimal).
volatile bool    resultPending = false;
volatile uint8_t resultCode    = 0;
char             resultDetail[64];

// Status/debug text lines from the PC board, queued in the recv callback
// and drained in loop(). A small ring buffer (rather than a single pending
// slot, as above) because several of these can arrive back-to-back faster
// than loop() drains one at a time (e.g. "credentials received" immediately
// followed by "typing bootstrap command...").
#define STATUS_QUEUE_LEN 8
char             statusQueue[STATUS_QUEUE_LEN][64];
volatile uint8_t statusQueueHead = 0;  // next slot to write (recv callback)
volatile uint8_t statusQueueTail = 0;  // next slot to read (loop)

void queueStatus(const char *text) {
  uint8_t next = (statusQueueHead + 1) % STATUS_QUEUE_LEN;
  if (next == statusQueueTail) return;  // queue full, drop rather than block
  strncpy(statusQueue[statusQueueHead], text, sizeof(statusQueue[0]) - 1);
  statusQueue[statusQueueHead][sizeof(statusQueue[0]) - 1] = '\0';
  statusQueueHead = next;
}

// -------- STATUS LED (XIAO ESP32-S3 onboard LED, active LOW) --------
#define STATUS_LED_PIN        LED_BUILTIN
#define STATUS_LED_ACTIVE_LOW true

void ledWrite(bool on) {
  digitalWrite(STATUS_LED_PIN, (on != STATUS_LED_ACTIVE_LOW) ? HIGH : LOW);
}

// -------- STATUS MATRIX (8x8 NeoMatrix on GPIO 14) --------
// Persistent baseline shows the ESP-NOW link to the PC board: dim red
// (on, not linked) or dim green (linked). A brief brighter flash overlays
// that baseline for one-shot events (credentials sent, result received).
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

enum EventFlash { EVT_NONE, EVT_SENT, EVT_RESULT_OK, EVT_RESULT_FAIL };
EventFlash          activeEventFlash  = EVT_NONE;
unsigned long        eventFlashStartMs = 0;
const unsigned long EVENT_FLASH_MS    = 400;  // how long a one-shot flash stays on top

void startEventFlash(EventFlash evt) {
  activeEventFlash  = evt;
  eventFlashStartMs = millis();
}

// Renders every loop: a one-shot event flash if one is active/recent,
// otherwise the persistent link-status baseline.
void renderMatrix() {
  uint8_t r, g, b;
  uint8_t brightness;

  bool flashing = (activeEventFlash != EVT_NONE) &&
                   (millis() - eventFlashStartMs < EVENT_FLASH_MS);
  if (activeEventFlash != EVT_NONE && !flashing) {
    activeEventFlash = EVT_NONE;  // flash window elapsed
  }

  if (flashing) {
    brightness = 150;
    switch (activeEventFlash) {
      case EVT_SENT:        r = 0;   g = 160; b = 255; break;  // cyan: credentials in flight
      case EVT_RESULT_OK:   r = 0;   g = 255; b = 0;   break;  // green: PC confirmed
      case EVT_RESULT_FAIL: r = 255; g = 0;   b = 0;   break;  // red: PC failed
      default:               r = 0;   g = 0;   b = 0;   break;
    }
  } else {
    brightness = 40;
    if (linkUp) { r = 0;  g = 255; b = 0; }  // dim green: linked to PC board
    else        { r = 255; g = 0;  b = 0; }  // dim red: on, not linked yet
  }

  matrix.setBrightness(brightness);
  matrix.fillScreen(matrix.Color(r, g, b));
  matrix.show();
}

// Send-status callback (optional, for debugging delivery).
// Signature differs across Arduino-ESP32 core versions.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  Serial.print("# ESP-NOW send: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

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

  if (len == sizeof(WifiResultMessage)) {
    WifiResultMessage res;
    memcpy(&res, data, sizeof(res));
    if (res.cmd != WIFI_RESULT_MAGIC) return;
    res.detail[sizeof(res.detail) - 1] = '\0';  // guard against a bad payload
    resultCode = res.result;
    memcpy(resultDetail, res.detail, sizeof(resultDetail));
    resultPending = true;
    return;
  }

  if (len == sizeof(StatusMessage)) {
    StatusMessage st;
    memcpy(&st, data, sizeof(st));
    if (st.cmd != STATUS_MAGIC) return;
    st.text[sizeof(st.text) - 1] = '\0';  // guard against a bad payload
    queueStatus(st.text);
    return;
  }
}

void espNowBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("# This board STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW init FAILED");
    return;
  }

  esp_now_register_send_cb(onEspNowSent);
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, espNowPeerMac, 6);
  peer.channel = 0;  // use the current WiFi channel
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("# ESP-NOW add peer FAILED");
    return;
  }

  espNowReady = true;
  Serial.println("# ESP-NOW ready");
}

void espNowSendHello() {
  if (!espNowReady) return;
  HandshakeMessage hs;
  hs.type = HS_HELLO;
  esp_now_send(espNowPeerMac, (const uint8_t *)&hs, sizeof(hs));
}

void updateLinkStatus() {
  unsigned long now = millis();

  if (now - lastHelloSentMs >= HELLO_INTERVAL_MS) {
    lastHelloSentMs = now;
    espNowSendHello();
  }

  bool connectedNow = (lastAckMs != 0) && (now - lastAckMs < LINK_TIMEOUT_MS);

  if (connectedNow && !linkUp) {
    linkUp = true;
    Serial.println("LINK:UP");
  } else if (!connectedNow && linkUp) {
    linkUp = false;
    Serial.println("LINK:DOWN");
  }

  ledWrite(linkUp);
}

// Parse "WIFI:<ssid>:<password>" and send it as a WifiCredMessage.
// ssid = everything between the first and second ':'.
// password = everything after the second ':' (may itself contain ':').
void handleWifiLine(const String &line) {
  int firstColon  = line.indexOf(':');
  int secondColon = (firstColon >= 0) ? line.indexOf(':', firstColon + 1) : -1;
  if (firstColon < 0 || secondColon < 0) {
    Serial.println("# ERR malformed WIFI line, expected WIFI:<ssid>:<password>");
    return;
  }

  String ssid = line.substring(firstColon + 1, secondColon);
  String pass = line.substring(secondColon + 1);

  if (ssid.length() == 0 || ssid.length() > 32) {
    Serial.println("# ERR ssid must be 1-32 chars");
    return;
  }
  if (pass.length() > 63) {
    Serial.println("# ERR password must be <= 63 chars");
    return;
  }

  WifiCredMessage msg = {};
  msg.cmd = WIFI_CRED_MAGIC;
  ssid.toCharArray(msg.ssid, sizeof(msg.ssid));
  pass.toCharArray(msg.password, sizeof(msg.password));

  if (!espNowReady) {
    Serial.println("# ERR ESP-NOW not ready");
    return;
  }

  esp_err_t r = esp_now_send(espNowPeerMac, (const uint8_t *)&msg, sizeof(msg));
  if (r == ESP_OK) {
    Serial.println("SENT");
    startEventFlash(EVT_SENT);
  } else {
    Serial.print("# ERR esp_now_send failed: ");
    Serial.println(r);
  }
}

// Requests the standalone HID self-test on the PC board (see TESTHID in
// handleSerial() below) — no credentials involved, just proves the PC
// board's keystrokes are landing in a focused, unlocked window.
void handleTestHidLine() {
  if (!espNowReady) {
    Serial.println("# ERR ESP-NOW not ready");
    return;
  }

  HandshakeMessage hs;
  hs.type = HS_SELFTEST_REQ;
  esp_err_t r = esp_now_send(espNowPeerMac, (const uint8_t *)&hs, sizeof(hs));
  if (r == ESP_OK) {
    Serial.println("TESTHID_SENT");
  } else {
    Serial.print("# ERR esp_now_send failed: ");
    Serial.println(r);
  }
}

void handleSerial() {
  static String lineBuf;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf.trim();
      if (lineBuf.startsWith("WIFI:")) {
        handleWifiLine(lineBuf);
      } else if (lineBuf == "TESTHID") {
        handleTestHidLine();
      } else if (lineBuf.length() > 0) {
        Serial.print("# ERR unknown command: ");
        Serial.println(lineBuf);
      }
      lineBuf = "";
    } else if (c != '\r') {
      lineBuf += c;
      if (lineBuf.length() > 160) lineBuf = "";  // guard against a runaway line
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(STATUS_LED_PIN, OUTPUT);
  ledWrite(false);

  matrix.begin();
  matrix.fillScreen(0);
  matrix.show();

  Serial.println("# WIFI_LOGIN_MAC_SENDER ready");
  espNowBegin();
}

void loop() {
  handleSerial();
  updateLinkStatus();

  while (statusQueueTail != statusQueueHead) {
    Serial.print("PC:");
    Serial.println(statusQueue[statusQueueTail]);
    statusQueueTail = (statusQueueTail + 1) % STATUS_QUEUE_LEN;
  }

  if (resultPending) {
    resultPending = false;
    if (resultCode == WIFI_RESULT_OK) {
      Serial.println("RESULT:OK");
      startEventFlash(EVT_RESULT_OK);
    } else {
      Serial.print("RESULT:FAIL:");
      Serial.println(resultDetail);
      startEventFlash(EVT_RESULT_FAIL);
    }
  }

  renderMatrix();
}
