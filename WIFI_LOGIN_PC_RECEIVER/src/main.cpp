/*
  Wi-Fi Login Bridge – PC RECEIVER (ESP32-S3 / Seeed XIAO ESP32-S3)

  Plugs into the target Windows PC over USB as a COMPOSITE device: a plain
  USB-CDC serial port plus a HID keyboard. This board is this PC's only
  connection to the outside world — no network, no other physical access —
  so there is no way to pre-install a listener script on it. Instead, when
  credentials arrive over ESP-NOW from the paired WIFI_LOGIN_MAC_SENDER
  board, this board TYPES a self-contained bootstrap command into Windows:

      Win+R -> "powershell ... -EncodedCommand <base64>" -> Enter

  The base64 blob decodes (PowerShell's -EncodedCommand expects UTF-16LE)
  to a short script that builds a WLAN profile XML, joins with `netsh wlan
  add profile` / `netsh wlan connect`, and — once it knows whether that
  worked — finds this board's own COM port and writes a result line back
  over the same USB link:

      RESULT:OK\n
      RESULT:FAIL:<detail>\n

  This board reads that line, packs it into a WifiResultMessage, and sends
  it back over ESP-NOW so the Mac side can confirm the PC actually joined.

  This board also sends its own status/debug lines (credentials received,
  bootstrap typing started/submitted, malformed input, etc.) to MAC_SENDER
  as StatusMessage, which prints them on the Mac as "PC:<text>". Since this
  board is normally plugged into a headless Windows PC with no other
  monitor, this is the only way to see what it's doing in real time.

  Two diagnostics exist specifically for that headless case, both visible
  only through the StatusMessage relay above:
    - USB mount state (tud_mounted()): reported whenever it changes. If
      this never flips to "mounted", Windows never completed USB
      enumeration at all — nothing HID- or serial-related can work
      regardless of screen/focus state, and the fault is the USB
      connection itself (cable/port/driver), not the typing logic.
    - HID self-test: a short, side-effect-free command (no netsh, no file
      writes) that types itself in, echoes a fixed marker back over this
      board's own COM port, and reports pass/fail. Runs automatically
      before every real credential bootstrap (no point typing ~5000
      characters blind into a window that already proved it isn't
      listening), and can also be triggered standalone from the Mac via
      the TESTHID serial command for fast iteration.

  Using -EncodedCommand means only base64 characters ever get typed — no
  quoting/escaping can go wrong during HID injection, and a corrupted
  keystroke just fails to decode (PowerShell errors out) rather than
  running something unintended.

  CAVEATS (inherent to blind keystroke injection — there is no way around
  these given this board is the only access to the PC):
    - Requires an unlocked, focused desktop session. A password-locked
      screen or screensaver that requires sign-in will swallow these
      keystrokes with no effect. A lone Shift press is sent first only to
      reset the *idle/screensaver timer*, not to defeat an actual lock.
    - If some other window has focus and intercepts Win+R, the typed text
      could land somewhere unexpected.
  This has been logic-tested (XML building, netsh-output parsing, the
  -EncodedCommand round trip) with PowerShell 7 on macOS, but the HID
  typing itself and the real Windows netsh flow have NOT been tested on
  real hardware — verify carefully before relying on it.

  Serial protocol (this board <-> the typed Windows script), newline-terminated:
    Windows -> board:  RESULT:OK | RESULT:FAIL:<detail>

  Status matrix (8x8 NeoMatrix on GPIO 14):
    dim red    - on, not yet linked to the MAC_SENDER board over ESP-NOW
    dim green  - linked (HELLO/ACK handshake with the MAC_SENDER board is current)
    cyan flash - credentials received; bootstrap command typed into Windows
    green flash- relayed a success result back to the MAC_SENDER board
    red flash  - relayed a failure result back to the MAC_SENDER board
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#include <USB.h>
#include <USBHIDKeyboard.h>
#include <tusb.h>

USBHIDKeyboard Keyboard;

// -------- KEYBOARD-RECOGNIZED DIAGNOSTIC (Caps Lock LED echo) --------
// Windows tracks Caps/Num/Scroll Lock state globally and sends an LED
// output report to every connected HID keyboard whenever it changes --
// this happens even at a lock screen, even with no window focused. If we
// press Caps Lock and get this report back, Windows' HID keyboard driver
// is genuinely bound and processing our input; if not, the keyboard
// interface itself is the problem, independent of anything Win+R-related.
volatile bool ledEventReceived = false;

void onKeyboardEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
    ledEventReceived = true;
  }
}

// Toggles Caps Lock and waits briefly for Windows' LED report. Also serves
// to restore Caps Lock to (approximately) its original state afterward.
bool testKeyboardRecognized() {
  ledEventReceived = false;
  Keyboard.press(KEY_CAPS_LOCK);
  delay(30);
  Keyboard.release(KEY_CAPS_LOCK);

  unsigned long deadline = millis() + 2000;
  while (millis() < deadline && !ledEventReceived) {
    delay(10);
  }
  bool recognized = ledEventReceived;

  // Toggle back off so we don't leave Caps Lock stuck on.
  delay(100);
  Keyboard.press(KEY_CAPS_LOCK);
  delay(30);
  Keyboard.release(KEY_CAPS_LOCK);

  return recognized;
}

// -------- WIFI CREDENTIAL MESSAGE (MAC_SENDER board -> this board) --------
// MUST match WIFI_LOGIN_MAC_SENDER's struct byte-for-byte.
#define WIFI_CRED_MAGIC 0xC1
typedef struct __attribute__((packed)) {
  uint8_t cmd;           // = WIFI_CRED_MAGIC
  char    ssid[33];      // up to 32 chars + NUL (802.11 max SSID length)
  char    password[64];  // up to 63 chars + NUL (WPA2-PSK max length)
} WifiCredMessage;

// -------- WIFI RESULT MESSAGE (this board -> MAC_SENDER board) --------
enum WifiResultCode { WIFI_RESULT_OK = 1, WIFI_RESULT_FAIL = 2 };
typedef struct __attribute__((packed)) {
  uint8_t cmd;        // = WIFI_RESULT_MAGIC
  uint8_t result;     // WifiResultCode
  char    detail[64]; // human-readable detail/error, NUL-terminated
} WifiResultMessage;
#define WIFI_RESULT_MAGIC 0xC2

// -------- STATUS MESSAGE (this board -> MAC_SENDER board) --------
// Free-form human-readable status/debug text, relayed by MAC_SENDER to the
// Mac as "PC:<text>" so its console shows what's happening on this board
// even though it's normally plugged into a headless Windows PC.
// MUST match WIFI_LOGIN_MAC_SENDER's struct byte-for-byte.
#define STATUS_MAGIC 0xC3
typedef struct __attribute__((packed)) {
  uint8_t cmd;      // = STATUS_MAGIC
  char    text[64]; // human-readable, NUL-terminated
} StatusMessage;

// -------- HANDSHAKE / LINK STATUS --------
// Mirrors the sender: it beacons HELLO, we reply ACK. HS_SELFTEST_REQ lets
// the Mac (via MAC_SENDER) trigger the short HID self-test on its own, for
// iterating on focus/lock-screen issues without waiting through a full
// credential bootstrap attempt each time.
enum HandshakeType { HS_HELLO = 1, HS_ACK = 2, HS_SELFTEST_REQ = 3 };
typedef struct __attribute__((packed)) {
  uint8_t type;
} HandshakeMessage;

// Broadcast back so the sender hears the ACK/result. esp_now_send needs a
// registered peer even for broadcast.
uint8_t broadcastAddr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

const unsigned long LINK_TIMEOUT_MS = 3000;  // no HELLO this long => link down

volatile unsigned long lastHelloMs = 0;  // last HELLO from sender (set in recv cb)
volatile bool          needAck     = false;
bool                    linkUp     = false;

// Credentials queued in the recv callback, printed to Serial from loop()
// (keep callback work minimal).
volatile bool credPending = false;
char          credSsid[33];
char          credPassword[64];

// Set in the recv callback on HS_SELFTEST_REQ, handled in loop().
volatile bool selfTestRequested = false;

// -------- STATUS LED (XIAO ESP32-S3 onboard LED, active LOW) --------
#define STATUS_LED_PIN        LED_BUILTIN
#define STATUS_LED_ACTIVE_LOW true

void ledWrite(bool on) {
  digitalWrite(STATUS_LED_PIN, (on != STATUS_LED_ACTIVE_LOW) ? HIGH : LOW);
}

// Forward declarations: defined further down, used by typeProvisionBootstrap()
// below before their definitions are reached.
void sendStatus(const String &text);
void sendResult(uint8_t result, const char *detail);

// -------- STATUS MATRIX (8x8 NeoMatrix on GPIO 14) --------
// Persistent baseline shows the ESP-NOW link to the MAC_SENDER board: dim
// red (on, not linked) or dim green (linked). A brief brighter flash
// overlays that baseline for one-shot events (credentials forwarded to
// Windows, result relayed back).
#define MATRIX_PIN    14
#define MATRIX_WIDTH  8
#define MATRIX_HEIGHT 8

Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(
  MATRIX_WIDTH, MATRIX_HEIGHT, MATRIX_PIN,
  NEO_MATRIX_TOP + NEO_MATRIX_LEFT +
  NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
  NEO_RGB + NEO_KHZ800
);

enum EventFlash { EVT_NONE, EVT_CRED_RECEIVED, EVT_RESULT_OK, EVT_RESULT_FAIL };
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
      case EVT_CRED_RECEIVED: r = 0;   g = 160; b = 255; break;  // cyan: forwarded to Windows
      case EVT_RESULT_OK:     r = 0;   g = 255; b = 0;   break;  // green: PC confirmed
      case EVT_RESULT_FAIL:   r = 255; g = 0;   b = 0;   break;  // red: PC failed
      default:                 r = 0;   g = 0;   b = 0;   break;
    }
  } else {
    brightness = 40;
    if (linkUp) { r = 0;  g = 255; b = 0; }  // dim green: linked to MAC_SENDER board
    else        { r = 255; g = 0;  b = 0; }  // dim red: on, not linked yet
  }

  matrix.setBrightness(brightness);
  matrix.fillScreen(matrix.Color(r, g, b));
  matrix.show();
}

// -------- WIFI PROVISION BOOTSTRAP (typed into Windows via HID) --------
// PowerShell script template, run on Windows via `-EncodedCommand` (base64
// of UTF-16LE text — see the comment at the top of this file for why).
// %%SSID%% / %%PASS%% are substituted with PowerShell-escaped values below.
// Mirrors the netsh/profile-XML logic that used to live in
// scripts/windows_wifi_provision.ps1, condensed into a single run: build
// the WLAN profile, join, poll for the connected state, then find this
// board's own COM port (matched by Espressif's USB VID 303A) and write
// the result back over it.
static const char PS_SCRIPT_TEMPLATE[] =
  R"PS($s='%%SSID%%';$p='%%PASS%%';$e={param($x)$x -replace '&','&amp;' -replace '<','&lt;' -replace '>','&gt;' -replace '"','&quot;' -replace "'",'&apos;'};$xs=&$e $s;if($p -eq ''){$sec="<authEncryption><authentication>open</authentication><encryption>none</encryption><useOneX>false</useOneX></authEncryption>"}else{$xp=&$e $p;$sec="<authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$xp</keyMaterial></sharedKey>"};$xml="<?xml version=`"1.0`"?><WLANProfile xmlns=`"http://www.microsoft.com/networking/WLAN/profile/v1`"><name>$xs</name><SSIDConfig><SSID><name>$xs</name></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security>$sec</security></MSM></WLANProfile>";$f="$env:TEMP\wlb.xml";Set-Content -Path $f -Value $xml -Encoding UTF8;netsh wlan add profile filename="$f" user=all | Out-Null;netsh wlan connect name="$s" | Out-Null;$ok=$false;for($i=0;$i -lt 12;$i++){Start-Sleep -Seconds 1;$sh=netsh wlan show interfaces | Out-String;$stm=[regex]::Match($sh,'State\s*:\s*(.+)');$csm=[regex]::Match($sh,'SSID\s*:\s*(.+)');if($stm.Success -and $csm.Success -and $stm.Groups[1].Value.Trim() -eq 'connected' -and $csm.Groups[1].Value.Trim() -eq $s){$ok=$true;break}};Remove-Item $f -ErrorAction SilentlyContinue;$r=if($ok){'RESULT:OK'}else{'RESULT:FAIL:timeout'};try{$d=Get-CimInstance Win32_PnPEntity|Where-Object{$_.Name -match '\(COM\d+\)' -and $_.PNPDeviceID -match 'VID_303A'}|Select-Object -First 1;if($d -and $d.Name -match '\((COM\d+)\)'){$c=$Matches[1];$sp=New-Object System.IO.Ports.SerialPort $c,115200,'None',8,'One';$sp.NewLine="`n";$sp.Open();$sp.WriteLine($r);Start-Sleep -Milliseconds 300;$sp.Close()}}catch{})PS";

static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64Encode(const uint8_t *data, size_t len) {
  String out;
  out.reserve(((len + 2) / 3) * 4 + 1);
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    out += B64_TABLE[(n >> 18) & 0x3F];
    out += B64_TABLE[(n >> 12) & 0x3F];
    out += B64_TABLE[(n >> 6) & 0x3F];
    out += B64_TABLE[n & 0x3F];
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t n = ((uint32_t)data[i]) << 16;
    out += B64_TABLE[(n >> 18) & 0x3F];
    out += B64_TABLE[(n >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out += B64_TABLE[(n >> 18) & 0x3F];
    out += B64_TABLE[(n >> 12) & 0x3F];
    out += B64_TABLE[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

// PowerShell's -EncodedCommand expects UTF-16LE. SSID/password are ASCII
// here (matches the ':'-delimited serial protocol's existing limitation);
// anything outside 7-bit ASCII is substituted with '?' rather than risking
// a garbled/undecodable command.
String utf16leEncode(const String &s) {
  String bytes;
  bytes.reserve(s.length() * 2);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    bytes += (char)((c < 0x80) ? c : '?');
    bytes += (char)0x00;
  }
  return bytes;
}

// Doubles any single quote so the value stays a single PowerShell token
// when embedded inside '...' in the script template above.
String psEscapeSingleQuoted(const String &s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}

String buildEncodedCommandLineFromScript(const String &script) {
  String utf16Bytes = utf16leEncode(script);
  String b64 = base64Encode((const uint8_t *)utf16Bytes.c_str(), utf16Bytes.length());

  String cmd = "powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ";
  cmd += b64;
  return cmd;
}

String buildEncodedCommandLine(const String &ssid, const String &password) {
  String script = PS_SCRIPT_TEMPLATE;
  script.replace("%%SSID%%", psEscapeSingleQuoted(ssid));
  script.replace("%%PASS%%", psEscapeSingleQuoted(password));
  return buildEncodedCommandLineFromScript(script);
}

// Short script for the HID self-test below: finds this board's own COM
// port (same VID-303A match as the real bootstrap script) and echoes a
// fixed marker back over it. No netsh, no file writes — just proves the
// keystrokes landed in an active PowerShell prompt and got executed.
static const char PS_SELFTEST_TEMPLATE[] =
  R"PS($d=Get-CimInstance Win32_PnPEntity|Where-Object{$_.Name -match '\(COM\d+\)' -and $_.PNPDeviceID -match 'VID_303A'}|Select-Object -First 1;if($d -and $d.Name -match '\((COM\d+)\)'){$c=$Matches[1];$sp=New-Object System.IO.Ports.SerialPort $c,115200,'None',8,'One';$sp.Open();$sp.WriteLine('HIDTEST:OK');Start-Sleep -Milliseconds 200;$sp.Close()})PS";

String buildSelfTestCommandLine() {
  return buildEncodedCommandLineFromScript(PS_SELFTEST_TEMPLATE);
}

void typeText(const String &text, unsigned long perCharDelayMs = 4) {
  for (size_t i = 0; i < text.length(); i++) {
    Keyboard.print(text[i]);
    delay(perCharDelayMs);
  }
}

void pressEnter() {
  delay(200);
  Keyboard.press(KEY_RETURN);
  delay(30);
  Keyboard.release(KEY_RETURN);
}

// A lone Shift resets the idle/screensaver timer without side effects in
// whatever currently has focus. Cannot defeat an actual password lock screen.
void wakeNudge() {
  Keyboard.press(KEY_LEFT_SHIFT);
  delay(20);
  Keyboard.release(KEY_LEFT_SHIFT);
  delay(300);
}

// Method A: Win+R -> cmdLine -> Enter. The Run dialog opens and steals
// focus regardless of what had it, when this works.
void typeViaRunDialog(const String &cmdLine) {
  wakeNudge();

  Keyboard.press(KEY_LEFT_GUI);
  delay(30);
  Keyboard.press('r');
  delay(30);
  Keyboard.releaseAll();
  delay(500);  // let the Run dialog open and take focus

  typeText(cmdLine);
  pressEnter();
}

// Method B: Ctrl+Esc (opens the Start Menu) -> "powershell" -> Enter to
// launch a console -> cmdLine -> Enter. A fallback for when Win+R
// specifically doesn't land: either Run/the Windows key is disabled by a
// policy on this PC, or (untested territory even after the Caps Lock
// check, which only proves *regular* keys are delivered) this library's
// encoding of the GUI modifier bit specifically has an issue. Ctrl+Esc is
// the standard OS-level alternate to the Windows key and doesn't touch it
// at all, so it sidesteps both possibilities.
void typeViaStartMenu(const String &cmdLine) {
  wakeNudge();

  Keyboard.press(KEY_LEFT_CTRL);
  delay(30);
  Keyboard.press(KEY_ESC);
  delay(30);
  Keyboard.releaseAll();
  delay(600);  // let the Start Menu open and its search box take focus

  typeText("powershell", 20);
  pressEnter();
  delay(1500);  // give the console host + powershell.exe time to launch and focus

  typeText(cmdLine);
  pressEnter();
}

// Blocks up to timeoutMs waiting for a line exactly matching `expected` on
// Serial. Only ever called right after typeCommandLine() while loop() is
// already blocked typing, so stealing bytes here doesn't race handleSerial().
bool waitForSerialLine(const String &expected, unsigned long timeoutMs) {
  unsigned long deadline = millis() + timeoutMs;
  String buf;
  while (millis() < deadline) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        buf.trim();
        if (buf == expected) return true;
        buf = "";
      } else if (c != '\r') {
        buf += c;
        if (buf.length() > 160) buf = "";
      }
    }
  }
  return false;
}

// Reports whether TinyUSB thinks Windows has completed USB enumeration at
// all (tud_mounted()) — this is a lower-level check than the self-test:
// it can be true even if HID typing never lands anywhere useful, but if
// it's FALSE, that alone explains everything (Windows never even saw this
// as a valid USB device, so no keystrokes could possibly land regardless
// of screen/focus state). The only signal available on a fully headless PC
// before attempting to type anything.
void reportUsbMountState() {
  bool mounted = tud_mounted();
  Serial.print("# USB device state: ");
  Serial.println(mounted ? "mounted" : "NOT mounted");
  sendStatus(String("USB device state: ") + (mounted ? "mounted" : "NOT mounted (Windows hasn't enumerated this device - check cable/port, or Device Manager if you can see it)"));
}

// Remembers whichever method last got a confirmed echo back, so the real
// (much longer) bootstrap command goes straight to what's known to work
// instead of re-trying Win+R first every time.
enum TypingMethod { METHOD_RUN_DIALOG, METHOD_START_MENU };
TypingMethod lastWorkingMethod = METHOD_RUN_DIALOG;

// Types the short self-test command via Win+R, and if that doesn't come
// back, retries via the Ctrl+Esc Start Menu fallback. Confirms a focused,
// unlocked PowerShell prompt was reached by SOME method and that typed
// keystrokes are being executed at all — the two things most likely to
// silently fail with blind keystroke injection.
bool typeHidSelfTest() {
  reportUsbMountState();

  bool kbRecognized = testKeyboardRecognized();
  Serial.println(kbRecognized ? "# HID keyboard: Windows acknowledged it (LED report received)"
                               : "# HID keyboard: NO response from Windows (no LED report)");
  sendStatus(kbRecognized
      ? "HID keyboard: Windows acknowledged it (LED report received)"
      : "HID keyboard: NO response from Windows - the keyboard interface likely isn't "
        "recognized even though USB mounted (this is independent of screen/focus state)");

  Serial.println("# HID self-test: trying Win+R...");
  sendStatus("HID self-test: trying Win+R...");
  typeViaRunDialog(buildSelfTestCommandLine());

  if (waitForSerialLine("HIDTEST:OK", 12000)) {
    Serial.println("# HID self-test: PASS (Win+R)");
    sendStatus("HID self-test: PASS (Win+R)");
    lastWorkingMethod = METHOD_RUN_DIALOG;
    return true;
  }

  Serial.println("# HID self-test: Win+R failed, trying Start Menu (Ctrl+Esc) fallback...");
  sendStatus("HID self-test: Win+R failed, trying Start Menu (Ctrl+Esc) fallback...");
  typeViaStartMenu(buildSelfTestCommandLine());

  bool ok = waitForSerialLine("HIDTEST:OK", 12000);
  Serial.println(ok ? "# HID self-test: PASS (Start Menu)" : "# HID self-test: FAIL (both methods)");
  sendStatus(ok ? "HID self-test: PASS (Start Menu fallback)"
                : "HID self-test: FAIL - neither Win+R nor Start Menu landed "
                  "(check focus/lock screen, or a policy may be blocking both)");
  if (ok) lastWorkingMethod = METHOD_START_MENU;
  return ok;
}

// Runs the self-test first; only proceeds to type the real (much longer,
// netsh-touching) bootstrap command if that passes. No point typing ~5000
// characters blind into a window that already proved it isn't listening.
// Uses whichever method the self-test just confirmed works.
void typeProvisionBootstrap(const String &ssid, const String &password) {
  if (!typeHidSelfTest()) {
    sendResult(WIFI_RESULT_FAIL, "HID self-test failed via both Win+R and Start Menu");
    startEventFlash(EVT_RESULT_FAIL);
    return;
  }

  String cmdLine = buildEncodedCommandLine(ssid, password);

  Serial.print("# typing bootstrap command (");
  Serial.print(cmdLine.length());
  Serial.println(" chars)...");
  sendStatus("typing bootstrap command (" + String(cmdLine.length()) + " chars)...");

  if (lastWorkingMethod == METHOD_START_MENU) {
    typeViaStartMenu(cmdLine);
  } else {
    typeViaRunDialog(cmdLine);
  }

  Serial.println("# bootstrap command submitted");
  sendStatus("bootstrap command submitted");
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(HandshakeMessage)) {
    HandshakeMessage hs;
    memcpy(&hs, data, sizeof(hs));
    if (hs.type == HS_HELLO) {
      lastHelloMs = millis();
      needAck     = true;  // reply from loop(), not here
    } else if (hs.type == HS_SELFTEST_REQ) {
      selfTestRequested = true;  // handled in loop(), not here
    }
    return;
  }

  if (len == sizeof(WifiCredMessage)) {
    WifiCredMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.cmd != WIFI_CRED_MAGIC) return;
    msg.ssid[sizeof(msg.ssid) - 1]         = '\0';  // guard against a bad payload
    msg.password[sizeof(msg.password) - 1] = '\0';
    memcpy(credSsid, msg.ssid, sizeof(credSsid));
    memcpy(credPassword, msg.password, sizeof(credPassword));
    credPending = true;
    return;
  }
}

void sendAck() {
  HandshakeMessage hs;
  hs.type = HS_ACK;
  esp_now_send(broadcastAddr, (const uint8_t *)&hs, sizeof(hs));
}

void sendResult(uint8_t result, const char *detail) {
  WifiResultMessage res = {};
  res.cmd    = WIFI_RESULT_MAGIC;
  res.result = result;
  strncpy(res.detail, detail, sizeof(res.detail) - 1);
  esp_now_send(broadcastAddr, (const uint8_t *)&res, sizeof(res));
}

// Relays a free-form status/debug line to the MAC_SENDER board, which
// prints it on the Mac as "PC:<text>" (truncated to fit StatusMessage.text
// if longer). Lets someone watching the Mac's serial console see what's
// happening on this board even though it's normally plugged into a
// headless Windows PC with no other visibility into it.
void sendStatus(const String &text) {
  StatusMessage msg = {};
  msg.cmd = STATUS_MAGIC;
  text.toCharArray(msg.text, sizeof(msg.text));
  esp_now_send(broadcastAddr, (const uint8_t *)&msg, sizeof(msg));
}

void updateLinkStatus() {
  unsigned long now = millis();
  bool connectedNow = (lastHelloMs != 0) && (now - lastHelloMs < LINK_TIMEOUT_MS);

  if (connectedNow && !linkUp) {
    linkUp = true;
    Serial.println("# LINK UP (HELLO received)");
  } else if (!connectedNow && linkUp) {
    linkUp = false;
    Serial.println("# LINK DOWN (no HELLO)");
  }

  ledWrite(linkUp);
}

// Reports tud_mounted() only on change, so it shows up on the Mac console
// as soon as Windows completes USB enumeration (or if it ever drops) —
// without ever having to type anything. On a fully headless PC this is the
// only baseline signal available: if it never flips to "mounted", nothing
// downstream (HID or the COM port) can possibly work regardless of screen
// state, and the problem is the USB connection itself (cable/port/driver).
bool usbMountedLast = false;
bool usbMountedEverReported = false;

void updateUsbMountStatus() {
  bool mounted = tud_mounted();
  if (mounted != usbMountedLast || !usbMountedEverReported) {
    usbMountedLast         = mounted;
    usbMountedEverReported = true;
    reportUsbMountState();
  }
}

// Parse "RESULT:OK" or "RESULT:FAIL:<detail>" from Windows and relay it
// back to the Mac board over ESP-NOW.
void handleResultLine(const String &line) {
  String rest = line.substring(String("RESULT:").length());
  if (rest.startsWith("OK")) {
    sendResult(WIFI_RESULT_OK, "");
    Serial.println("# relayed RESULT:OK");
    startEventFlash(EVT_RESULT_OK);
  } else if (rest.startsWith("FAIL")) {
    int colon = rest.indexOf(':');
    String detail = (colon >= 0) ? rest.substring(colon + 1) : "unknown error";
    sendResult(WIFI_RESULT_FAIL, detail.c_str());
    Serial.print("# relayed RESULT:FAIL:");
    Serial.println(detail);
    startEventFlash(EVT_RESULT_FAIL);
  } else {
    Serial.println("# ERR malformed RESULT line");
    sendStatus("ERR malformed RESULT line from Windows: " + line);
  }
}

void handleSerial() {
  static String lineBuf;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf.trim();
      if (lineBuf.startsWith("RESULT:")) {
        handleResultLine(lineBuf);
      } else if (lineBuf.length() > 0) {
        Serial.print("# ERR unknown command: ");
        Serial.println(lineBuf);
        sendStatus("ERR unknown command from Windows: " + lineBuf);
      }
      lineBuf = "";
    } else if (c != '\r') {
      lineBuf += c;
      if (lineBuf.length() > 160) lineBuf = "";  // guard against a runaway line
    }
  }
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("# Receiver STA MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW init FAILED");
    return;
  }

  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, 6);
  peer.channel = 0;  // use the current WiFi channel
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("# ESP-NOW add peer FAILED");
    return;  // no peer registered, sendStatus() below would just fail silently
  }

  Serial.println("# ESP-NOW ready - waiting for Wi-Fi credentials");
  sendStatus("PC_RECEIVER ESP-NOW ready, waiting for Wi-Fi credentials");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(STATUS_LED_PIN, OUTPUT);
  ledWrite(false);

  matrix.begin();
  matrix.fillScreen(0);
  matrix.show();

  Keyboard.begin();
  Keyboard.onEvent(onKeyboardEvent);
  USB.begin();

  Serial.println("# WIFI_LOGIN_PC_RECEIVER ready");
  initEspNow();
}

void loop() {
  if (needAck) {
    needAck = false;
    sendAck();
  }

  if (selfTestRequested) {
    selfTestRequested = false;
    bool ok = typeHidSelfTest();
    startEventFlash(ok ? EVT_RESULT_OK : EVT_RESULT_FAIL);
  }

  if (credPending) {
    credPending = false;
    Serial.print("# received credentials for '");
    Serial.print(credSsid);
    Serial.println("'");
    sendStatus("received credentials for '" + String(credSsid) + "'");
    startEventFlash(EVT_CRED_RECEIVED);

    // Also print the plain WIFI:<ssid>:<password> line: harmless if nothing
    // is listening, but keeps scripts/windows_wifi_provision.ps1 usable as
    // the alternate path for anyone who got one-time access to pre-install
    // it as a persistent Startup-folder listener (see that script's header).
    Serial.print("WIFI:");
    Serial.print(credSsid);
    Serial.print(":");
    Serial.println(credPassword);

    typeProvisionBootstrap(credSsid, credPassword);
  }

  handleSerial();
  updateLinkStatus();
  updateUsbMountStatus();
  renderMatrix();
}
