# Wi-Fi Login Bridge

Gets a headless, auto-login Windows PC onto Wi-Fi from a Mac, using two
ESP32-S3 boards as a wireless bridge, when the ESP32 plugged into the
Windows PC is the *only* access to that machine (no network, no other
physical/remote access). The Mac sends the SSID/password over the bridge;
the PC_RECEIVER board bootstraps the connection on Windows itself.

```
   Mac                                                      Windows PC
    |                                                            |
  USB serial                                            USB serial + HID
    |                                                            |
WIFI_LOGIN_MAC_SENDER  ===  ESP-NOW (WiFi radio)  ===  WIFI_LOGIN_PC_RECEIVER
 (this project)                                          (sibling project)
```

1. `mac_wifi_login.py` (this project's `scripts/`) writes `WIFI:<ssid>:<password>`
   over USB serial to the MAC_SENDER board.
2. MAC_SENDER packs it into a struct and sends it over ESP-NOW to PC_RECEIVER.
3. PC_RECEIVER — which enumerates on Windows as BOTH a serial port and a USB
   HID keyboard — types a single bootstrap command into Windows: `Win+R` ->
   `powershell ... -EncodedCommand <base64>` -> `Enter`. That command is a
   self-contained script (nothing needs to pre-exist on the PC) that builds
   a WLAN profile and runs `netsh wlan add profile` / `netsh wlan connect`.
   Using `-EncodedCommand` means only base64 characters ever get typed — no
   fragile quoting, and a dropped keystroke just fails to decode rather than
   running something unintended.
4. Once that script knows whether the PC joined, it finds PC_RECEIVER's own
   COM port and writes `RESULT:OK` / `RESULT:FAIL:<detail>` back over it.
5. PC_RECEIVER relays that to MAC_SENDER over ESP-NOW, which relays it to
   the Mac, so the Python script can confirm the PC actually joined.

Both boards also exchange a HELLO/ACK handshake so each side's 8x8 LED
matrix (GPIO 14) shows whether the ESP-NOW link to the other board is up —
dim red = on/not linked, dim green = linked, brief colored flashes for
events (see each board's `src/main.cpp` header for the exact color key).

PC_RECEIVER is normally plugged into a headless Windows PC with no other
monitor, so it also relays its own status/debug lines (credentials
received, bootstrap command typing started/submitted, malformed input,
etc.) to MAC_SENDER over ESP-NOW. MAC_SENDER prints these on the Mac as
`PC:<text>`, so `mac_wifi_login.py` — or anyone just watching MAC_SENDER's
serial monitor — can see what PC_RECEIVER is doing in real time, not just
the final `RESULT:OK` / `RESULT:FAIL`.

**If you do get one-time physical/remote access to the Windows PC** before
it's deployed, `../WIFI_LOGIN_PC_RECEIVER/scripts/windows_wifi_provision.ps1`
is a more robust alternative: drop it in the Startup folder once and it runs
as a persistent listener, no HID typing required. The HID bootstrap above
exists specifically for when that one-time access isn't available.

## Hardware

- 2x ESP32-S3 board (targets `seeed_xiao_esp32s3`; any ESP32-S3 with native
  USB works, just change `board` in each `platformio.ini`).
- One plugged into the Mac (flash **this** project, `WIFI_LOGIN_MAC_SENDER`).
- One plugged into the Windows PC (flash `../WIFI_LOGIN_PC_RECEIVER`).
- Both boards need to be in ESP-NOW range of each other (same room is fine;
  this does NOT need to be close to the Windows PC's real range/location —
  ESP-NOW range is the constraint, not proximity to the PC).
- Use a data-capable USB cable on both ends (not charge-only).

## Flashing

```
pio run -t upload -e seeed_xiao_esp32s3   # from each project directory
```

## Usage

1. Flash both boards, plug MAC_SENDER into the Mac and PC_RECEIVER into the
   Windows PC.
2. On the Mac:
   ```
   pip install pyserial
   python3 scripts/mac_wifi_login.py --ssid "Hotel WiFi" --password "s3cret"
   ```
   Omit `--password` to be prompted, or pass `--open` for a network with no
   password. This can take up to ~1-2 minutes end-to-end (typing the
   bootstrap command out is the slow part), so the default `--timeout` is
   generous — pass a larger one if needed.

The Python script prints the boards' debug/link-status lines as they arrive
and exits 0 on `RESULT:OK`, non-zero on failure or timeout.

### Important caveats of the HID bootstrap

Blind keystroke injection has real limits, inherent to having no other way
to reach the PC:

- **Requires an unlocked, focused desktop session.** A password lock
  screen or a screensaver requiring sign-in will swallow the keystrokes
  with no effect (auto-login only covers the *initial* boot). PC_RECEIVER
  sends a lone Shift press first to reset the idle timer, but that cannot
  defeat an actual lock screen.
- **Whatever has focus receives the keystrokes.** If some other window
  intercepts `Win+R`, the typed text could land somewhere unexpected.
- This has been logic-tested (XML building, `netsh`-output parsing, the
  `-EncodedCommand` round trip) with PowerShell 7 on macOS — but the actual
  HID typing and the real Windows `netsh` flow have **not** been verified
  on real hardware. Test carefully.

## Protocol reference

Serial lines are newline-terminated ASCII. `:` separates fields on the
Mac<->MAC_SENDER link, so an SSID may not contain `:` (the password may —
it's everything after the second `:`).

| Link | Line | Meaning |
|---|---|---|
| Mac -> MAC_SENDER | `WIFI:<ssid>:<password>` | request a connection |
| MAC_SENDER -> Mac | `LINK:UP` / `LINK:DOWN` | ESP-NOW link to PC_RECEIVER |
| MAC_SENDER -> Mac | `SENT` | credentials handed to ESP-NOW |
| Windows -> PC_RECEIVER | `RESULT:OK` | PC joined the network |
| Windows -> PC_RECEIVER | `RESULT:FAIL:<detail>` | PC failed to join |
| MAC_SENDER -> Mac | `RESULT:OK` / `RESULT:FAIL:<detail>` | relayed final result |
| MAC_SENDER -> Mac | `PC:<text>` | status/debug line relayed from PC_RECEIVER |

Lines starting with `#` on any link are debug logging and can be ignored.

ESP-NOW messages are C structs shared (and kept in sync by hand, commented
at each definition) between the two firmware projects: `WifiCredMessage`,
`WifiResultMessage`, `StatusMessage` (PC_RECEIVER's status/debug text,
relayed as `PC:<text>`), and a small `HandshakeMessage` for link status.
