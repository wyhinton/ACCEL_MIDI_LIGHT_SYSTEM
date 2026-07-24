#!/usr/bin/env python3
"""
Send Wi-Fi credentials to a headless Windows PC over the ESP32 bridge.

Talks to the WIFI_LOGIN_MAC_SENDER board over USB serial, which relays the
credentials over ESP-NOW to a WIFI_LOGIN_PC_RECEIVER board plugged into the
Windows PC. That board types a self-contained bootstrap command into
Windows (Win+R -> powershell -EncodedCommand ...) that joins the network
with `netsh` and reports back success/failure, which is relayed all the
way back here. Typing that command out can take up to a minute or so, on
top of the time `netsh` itself takes -- give it a generous --timeout.

Before sending real credentials, it's worth running --test-hid: the PC
board types a short, self-contained command that only echoes a marker back
over serial (no netsh, no file writes). If that doesn't come back within
~12s, the real bootstrap almost certainly won't land either (locked/wrong
focus), and you've found out in ~15s instead of after the full ~150s
timeout below.

Usage:
    python3 mac_wifi_login.py --test-hid                    # quick sanity check first
    python3 mac_wifi_login.py --ssid "Hotel WiFi" --password "s3cret"
    python3 mac_wifi_login.py --ssid "Hotel WiFi"           # prompts for password
    python3 mac_wifi_login.py --list-ports                  # show candidate ports

Requires: pip install pyserial
"""

import argparse
import getpass
import sys
import time

import serial
import serial.tools.list_ports

ESPRESSIF_USB_VID = 0x303A  # USB vendor ID used by native ESP32-S3 USB-CDC


def find_default_port():
    candidates = [
        p for p in serial.tools.list_ports.comports() if p.vid == ESPRESSIF_USB_VID
    ]
    if len(candidates) == 1:
        return candidates[0].device
    return None


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        vid = f"{p.vid:04X}" if p.vid else "----"
        print(f"  {p.device}  (VID:{vid}  {p.description})")


class ResilientSerial:
    """Wraps a serial connection to MAC_SENDER, transparently reopening it
    if the OS drops the connection mid-read. This board's native USB CDC
    has occasionally hit `SerialException: device reports readiness to
    read but returned no data` on macOS during heavy traffic (e.g. the
    ESP-NOW send-status logging while HELLO beacons are chattering) --
    reconnecting rather than crashing keeps a long-running wait (like the
    ~150s credential timeout) from being derailed by a transient hiccup.
    """

    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.ser = None
        self._open()

    def _open(self):
        print(f"Connecting to {self.port} @ {self.baud}...")
        self.ser = serial.Serial(self.port, self.baud, timeout=1)
        time.sleep(2)  # let the board finish its post-reset boot banner
        self.ser.reset_input_buffer()

    def write_line(self, line):
        self.ser.write(line.encode("utf-8"))

    def readline_text(self):
        """Returns one decoded, stripped line, or None if nothing arrived
        (including right after a transparent reconnect)."""
        try:
            raw = self.ser.readline()
        except serial.SerialException:
            print("  (serial connection hiccuped, reconnecting...)")
            try:
                self.ser.close()
            except Exception:
                pass
            time.sleep(0.5)
            self._open()
            return None
        if not raw:
            return None
        return raw.decode("utf-8", errors="replace").strip()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


def run_test_hid(port, baud, timeout):
    """Trigger the PC board's standalone HID self-test and report pass/fail."""
    with ResilientSerial(port, baud) as rs:
        rs.write_line("TESTHID\n")
        print("Requested HID self-test. Waiting for the PC board to type it "
              "and echo a marker back...")

        deadline = time.time() + timeout
        while time.time() < deadline:
            text = rs.readline_text()
            if not text:
                continue
            print(f"  {text}")

            if text == "PC:HID self-test: PASS":
                print("\nPASS: keystrokes are landing in a focused, unlocked window.")
                return
            if text.startswith("PC:HID self-test: FAIL"):
                print("\nFAIL: the typed command wasn't executed. Check the Windows "
                      "PC's screen state (locked? screensaver?) and that nothing else "
                      "has focus, then try again.")
                sys.exit(1)

        print("\nTimed out waiting for a self-test result. Check both boards show "
              "LINK:UP (steady green on the matrix) -- if they don't, this is an "
              "ESP-NOW link problem, not a keystroke problem.")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssid", help="Wi-Fi network name")
    parser.add_argument("--password", help="Wi-Fi password (omit for an open network, "
                                             "or you'll be prompted if not given)")
    parser.add_argument("--open", action="store_true",
                         help="Connect to an open (no-password) network")
    parser.add_argument("--port", help="Serial port for the MAC_SENDER board "
                                        "(auto-detected if omitted)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=150.0,
                         help="Seconds to wait for the PC to confirm connection "
                              "(the PC board has to type out a bootstrap command, "
                              "which alone can take ~30-60s, before netsh even runs)")
    parser.add_argument("--test-hid", action="store_true",
                         help="Just run the PC board's short HID self-test (no "
                              "credentials sent) and report pass/fail")
    parser.add_argument("--list-ports", action="store_true",
                         help="List candidate serial ports and exit")
    args = parser.parse_args()

    if args.list_ports:
        list_ports()
        return

    port = args.port or find_default_port()
    if not port:
        print("Could not auto-detect the MAC_SENDER board's serial port.", file=sys.stderr)
        print("Available ports:", file=sys.stderr)
        list_ports()
        print("\nPass one explicitly with --port", file=sys.stderr)
        sys.exit(1)

    if args.test_hid:
        run_test_hid(port, args.baud, timeout=20.0)
        return

    if not args.ssid:
        parser.error("--ssid is required (or use --test-hid / --list-ports)")

    if ":" in args.ssid:
        parser.error("SSID cannot contain ':' -- the serial protocol uses it as a separator")

    password = "" if args.open else args.password
    if password is None:
        password = getpass.getpass(f"Password for '{args.ssid}' (blank for open network): ")

    with ResilientSerial(port, args.baud) as rs:
        line = f"WIFI:{args.ssid}:{password}\n"
        rs.write_line(line)
        print(f"Sent credentials for '{args.ssid}'. Waiting for the PC to connect...")

        deadline = time.time() + args.timeout
        while time.time() < deadline:
            text = rs.readline_text()
            if not text:
                continue

            if text.startswith("#"):
                print(f"  {text}")
                continue

            print(f"  {text}")

            if text == "RESULT:OK":
                print(f"\nSuccess: the PC connected to '{args.ssid}'.")
                return
            if text.startswith("RESULT:FAIL"):
                detail = text.split(":", 2)[-1] if text.count(":") >= 2 else "unknown error"
                print(f"\nFailed: the PC could not connect to '{args.ssid}' ({detail}).")
                sys.exit(1)

        print("\nTimed out waiting for a result. Check that the PC board is plugged in "
              "and both boards show LINK:UP (steady green on the matrix). If it linked "
              "and flashed cyan but never resolved, the typed bootstrap command likely "
              "didn't land in an active Windows session (locked screen, wrong focus, etc).")
        sys.exit(1)


if __name__ == "__main__":
    main()
