# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Join the MULTIPLEX_8266 board's open Wi-Fi SoftAP from Windows, without
digging through the Settings app each time.

Drives the built-in `netsh wlan` CLI: writes a temporary open-network
profile for the board's SSID, adds it, connects, then polls until Windows
reports the link up (or times out). The profile is added in "manual" mode
so Windows won't auto-rejoin the board's AP later and silently steal your
internet connection -- see ARTNET_CONTROL.md, "the PC loses internet access
while connected unless it has a second network adapter."

Examples:
  uv run connect_wifi.py                  # join MULTIPLEX_LIGHTS, ping the board
  uv run connect_wifi.py --ssid OTHER_AP
  uv run connect_wifi.py --list-interfaces
  uv run connect_wifi.py --scan
  uv run connect_wifi.py --disconnect
"""

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_SSID = "MULTIPLEX_LIGHTS"  # AUDIO_AP_SSID in src/main.cpp
BOARD_IP = "192.168.4.1"
PROFILE_TEMPLATE = """<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
    <name>{ssid}</name>
    <SSIDConfig>
        <SSID>
            <name>{ssid}</name>
        </SSID>
    </SSIDConfig>
    <connectionType>ESS</connectionType>
    <connectionMode>manual</connectionMode>
    <MSM>
        <security>
            <authEncryption>
                <authentication>open</authentication>
                <encryption>none</encryption>
                <useOneX>false</useOneX>
            </authEncryption>
        </security>
    </MSM>
</WLANProfile>
"""


def run_netsh(*args):
    try:
        return subprocess.run(
            ["netsh", *args], capture_output=True, text=True, errors="replace"
        )
    except FileNotFoundError:
        sys.exit("netsh not found -- this script only works on Windows")


def parse_blocks(output):
    """netsh's "Key : Value" blocks, separated by blank lines, as dicts."""
    blocks, current = [], {}
    for line in output.splitlines():
        if not line.strip():
            if current:
                blocks.append(current)
                current = {}
            continue
        if ":" in line:
            key, _, value = line.partition(":")
            current[key.strip()] = value.strip()
    if current:
        blocks.append(current)
    return blocks


def list_interfaces():
    result = run_netsh("wlan", "show", "interfaces")
    return [b for b in parse_blocks(result.stdout) if "Name" in b]


def pick_interface(requested):
    interfaces = list_interfaces()
    if not interfaces:
        sys.exit("No Wi-Fi interfaces found -- is Wi-Fi enabled/adapter present?")
    if requested:
        for iface in interfaces:
            if iface["Name"].lower() == requested.lower():
                return iface["Name"]
        names = ", ".join(repr(i["Name"]) for i in interfaces)
        sys.exit(f"No interface named {requested!r} -- available: {names}")
    if len(interfaces) > 1:
        names = ", ".join(repr(i["Name"]) for i in interfaces)
        print(f"Multiple Wi-Fi interfaces found ({names}); using {interfaces[0]['Name']!r} "
              "(pass --interface to pick another)")
    return interfaces[0]["Name"]


def scan_ssids(iface):
    run_netsh("wlan", "show", "networks", f"interface={iface}")  # nudge a fresh scan
    result = run_netsh("wlan", "show", "networks", f"interface={iface}")
    ssids = []
    for line in result.stdout.splitlines():
        if line.strip().startswith("SSID "):
            ssids.append(line.partition(":")[2].strip())
    return ssids


def current_state(iface):
    for block in list_interfaces():
        if block.get("Name") == iface:
            return block.get("State", ""), block.get("SSID", "")
    return "", ""


def add_profile(ssid, iface):
    xml = PROFILE_TEMPLATE.format(ssid=ssid)
    with tempfile.NamedTemporaryFile(
        "w", suffix=".xml", delete=False, encoding="utf-8"
    ) as f:
        f.write(xml)
        path = Path(f.name)
    try:
        result = run_netsh(
            "wlan", "add", "profile", f"filename={path}", f"interface={iface}", "user=current"
        )
        if result.returncode != 0:
            sys.exit(f"Could not add Wi-Fi profile for {ssid!r}:\n{result.stdout or result.stderr}")
    finally:
        path.unlink(missing_ok=True)


def ping_board(ip):
    print(f"Pinging board at {ip} ...")
    result = subprocess.run(
        ["ping", "-n", "3", ip], capture_output=True, text=True, errors="replace"
    )
    print(result.stdout.strip())
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--ssid", default=DEFAULT_SSID,
                        help=f"network to join (default: {DEFAULT_SSID!r})")
    parser.add_argument("--interface", metavar="NAME",
                        help="Wi-Fi adapter name (default: autodetect; see --list-interfaces)")
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="seconds to wait for the connection to come up (default 15)")
    parser.add_argument("--no-ping", action="store_true",
                        help=f"skip pinging the board ({BOARD_IP}) after connecting")
    parser.add_argument("--board-ip", default=BOARD_IP,
                        help=f"board IP to ping (default: {BOARD_IP})")
    parser.add_argument("--list-interfaces", action="store_true",
                        help="list Wi-Fi adapters and their current state, then exit")
    parser.add_argument("--scan", action="store_true",
                        help="list SSIDs currently visible to the adapter, then exit")
    parser.add_argument("--disconnect", action="store_true",
                        help="disconnect the adapter and exit (no reconnect)")
    args = parser.parse_args()

    if args.list_interfaces:
        for iface in list_interfaces():
            print(f"{iface['Name']}: state={iface.get('State', '?')} "
                  f"ssid={iface.get('SSID', '-')}")
        return

    iface = pick_interface(args.interface)

    if args.disconnect:
        run_netsh("wlan", "disconnect", f"interface={iface}")
        print(f"Disconnected {iface!r}")
        return

    if args.scan:
        ssids = scan_ssids(iface)
        print("\n".join(ssids) if ssids else "(no networks visible)")
        return

    state, ssid = current_state(iface)
    if state == "connected" and ssid == args.ssid:
        print(f"{iface!r} is already connected to {args.ssid!r}")
    else:
        visible = scan_ssids(iface)
        if args.ssid not in visible:
            print(f"Warning: {args.ssid!r} isn't currently visible to {iface!r} "
                  "-- is the board powered on and in range? Trying anyway...")

        add_profile(args.ssid, iface)
        result = run_netsh(
            "wlan", "connect", f"name={args.ssid}", f"ssid={args.ssid}", f"interface={iface}"
        )
        if result.returncode != 0:
            sys.exit(f"netsh wlan connect failed:\n{result.stdout or result.stderr}")

        print(f"Joining {args.ssid!r} on {iface!r} ", end="", flush=True)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            state, ssid = current_state(iface)
            if state == "connected" and ssid == args.ssid:
                break
            print(".", end="", flush=True)
            time.sleep(0.5)
        print()

        if state != "connected" or ssid != args.ssid:
            sys.exit(f"Timed out waiting to join {args.ssid!r} (last state: "
                     f"{state or 'unknown'!r}, ssid: {ssid or 'none'!r})")
        print(f"Connected to {args.ssid!r}")

    if not args.no_ping:
        if not ping_board(args.board_ip):
            print(f"Warning: joined the network but {args.board_ip} isn't responding to ping "
                  "yet -- give the board a few seconds, or check it's actually up.")


if __name__ == "__main__":
    main()
