# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pycaw",
#     "comtypes",
#     "pyserial",
# ]
# ///
"""Stream a Windows output device's live audio level to the MULTIPLEX_8266
lights, over USB serial and/or the board's Wi-Fi SoftAP (UDP).

The level rides the Windows endpoint peak meter (the Volume Mixer's green
bars, via IAudioMeterInformation) -- no audio is captured, so it works no
matter which app is playing, at near-zero CPU. Each reading is smoothed
with an attack/release envelope, mapped through a dB window and a gamma
curve, and sent as a [0xAD][level 0-255] frame. The board treats it as a
master brightness scale; its 'o'/'l' keys (or --depth here) set how
strongly. Entirely optional: the board snaps back to full brightness one
second after this script stops.

Examples:
  uv run audio_level.py --list
  uv run audio_level.py --serial-port COM5
  uv run audio_level.py --udp                    # SoftAP default 192.168.4.1:7777
  uv run audio_level.py --device "USB Audio" --udp --depth 80

In serial mode this doubles as a terminal: the board's output is printed
and your keystrokes are forwarded, since the COM port can't be shared with
a separate monitor. Over --udp the COM port stays free.
"""

import argparse
import math
import msvcrt
import os
import socket
import sys
import time
from ctypes import POINTER, cast

import serial
from comtypes import CLSCTX_ALL
from pycaw.constants import DEVICE_STATE, EDataFlow
from pycaw.pycaw import AudioUtilities, IAudioMeterInformation

LEVEL_SYNC = 0xAD
DEPTH_SYNC = 0xAE
DEFAULT_UDP_HOST = "192.168.4.1"  # the board's SoftAP address
DEFAULT_UDP_PORT = 7777
SERIAL_BAUD = 115200
DEPTH_RESEND_S = 1.0  # so a board that reboots mid-session picks --depth back up
BAR_WIDTH = 24


def enumerate_render_devices():
    """Active render (output) endpoints as (AudioDevice, IMMDevice) pairs."""
    enumerator = AudioUtilities.GetDeviceEnumerator()
    collection = enumerator.EnumAudioEndpoints(
        EDataFlow.eRender.value, DEVICE_STATE.ACTIVE.value
    )
    devices = []
    for i in range(collection.GetCount()):
        imm = collection.Item(i)
        devices.append((AudioUtilities.CreateDevice(imm), imm))
    return devices


def open_meter(device_substr):
    """The peak meter for the chosen device (default: default output)."""
    if device_substr is None:
        imm = AudioUtilities.GetSpeakers()
        name = "default output device"
    else:
        matches = [
            (dev, imm)
            for dev, imm in enumerate_render_devices()
            if device_substr.lower() in (dev.FriendlyName or "").lower()
        ]
        if not matches:
            sys.exit(f"No active output device matching {device_substr!r} -- try --list")
        if len(matches) > 1:
            names = ", ".join(repr(d.FriendlyName) for d, _ in matches)
            sys.exit(f"{device_substr!r} matches several devices ({names}) -- be more specific")
        dev, imm = matches[0]
        name = dev.FriendlyName
    interface = imm.Activate(IAudioMeterInformation._iid_, CLSCTX_ALL, None)
    return cast(interface, POINTER(IAudioMeterInformation)), name


def parse_udp_target(value):
    if ":" in value:
        host, port = value.rsplit(":", 1)
        return host, int(port)
    return value, DEFAULT_UDP_PORT


# ---- serial passthrough -----------------------------------------------------
# The COM port is exclusive, so while we hold it we also act as the terminal:
# board output is printed (whole lines only, so the meter line stays tidy)
# and console keystrokes are forwarded as the usual single-key commands.

serial_rx_buffer = ""


def pump_serial_output(ser):
    global serial_rx_buffer
    waiting = ser.in_waiting
    if not waiting:
        return
    serial_rx_buffer += ser.read(waiting).decode("utf-8", errors="replace")
    while "\n" in serial_rx_buffer:
        line, serial_rx_buffer = serial_rx_buffer.split("\n", 1)
        print("\r\x1b[K" + line.rstrip("\r"))


def pump_keys(ser):
    while msvcrt.kbhit():
        key = msvcrt.getch()
        if key in (b"\x00", b"\xe0"):  # arrow/function key prefix; swallow the pair
            msvcrt.getch()
            continue
        if key == b"\x03":  # Ctrl-C arrives as a key when the console is in getch mode
            raise KeyboardInterrupt
        ser.write(key)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--list", action="store_true", help="list active output devices and exit")
    parser.add_argument("--device", metavar="SUBSTR",
                        help="output device name substring (default: the default output device)")
    parser.add_argument("--serial-port", metavar="COMx",
                        help="send frames over this COM port (also acts as a serial terminal)")
    parser.add_argument("--udp", metavar="HOST[:PORT]", nargs="?", const=DEFAULT_UDP_HOST,
                        help=f"send frames over UDP; bare --udp targets the board's SoftAP "
                             f"({DEFAULT_UDP_HOST}:{DEFAULT_UDP_PORT})")
    parser.add_argument("--depth", type=int, metavar="PCT",
                        help="set the board's audio depth 0-100 (how strongly the level dims "
                             "the lights); omit to leave the board's own setting alone")
    parser.add_argument("--fps", type=float, default=60.0, help="meter polls per second (default 60)")
    parser.add_argument("--attack", type=float, default=15.0, metavar="MS",
                        help="envelope attack time (default 15ms; smaller = punchier)")
    parser.add_argument("--release", type=float, default=250.0, metavar="MS",
                        help="envelope release time (default 250ms; larger = smoother decay)")
    parser.add_argument("--floor-db", type=float, default=-45.0,
                        help="level at/below this many dBFS maps to dark (default -45)")
    parser.add_argument("--ceil-db", type=float, default=-6.0,
                        help="level at/above this many dBFS maps to full (default -6)")
    parser.add_argument("--gamma", type=float, default=1.8,
                        help="response curve; >1 keeps quiet passages dimmer (default 1.8)")
    args = parser.parse_args()

    if args.list:
        for dev, _ in enumerate_render_devices():
            print(dev.FriendlyName)
        return
    if not args.serial_port and args.udp is None:
        parser.error("give --serial-port COMx and/or --udp [HOST[:PORT]] (or --list)")
    if args.depth is not None and not 0 <= args.depth <= 100:
        parser.error("--depth must be 0-100")
    if args.ceil_db <= args.floor_db:
        parser.error("--ceil-db must be above --floor-db")

    meter, device_name = open_meter(args.device)

    ser = None
    if args.serial_port:
        try:
            ser = serial.Serial(args.serial_port, SERIAL_BAUD, timeout=0)
        except serial.SerialException as exc:
            sys.exit(f"Could not open {args.serial_port}: {exc} "
                     "(close any serial monitor holding the port)")
    sock = udp_addr = None
    if args.udp is not None:
        udp_addr = parse_udp_target(args.udp)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    targets = [t for t in (args.serial_port, udp_addr and f"{udp_addr[0]}:{udp_addr[1]}/udp") if t]
    print(f"Metering {device_name} -> {' + '.join(targets)} (Ctrl-C stops; "
          "the board reverts to full brightness ~1s after)")

    os.system("")  # enable ANSI escapes in the Windows console (for the meter line)

    dt = 1.0 / args.fps
    attack_coef = 1 - math.exp(-dt / (args.attack / 1000)) if args.attack > 0 else 1.0
    release_coef = 1 - math.exp(-dt / (args.release / 1000)) if args.release > 0 else 1.0
    envelope = 0.0
    last_depth_sent = 0.0

    def send(payload):
        if ser:
            ser.write(payload)
        if sock:
            try:
                sock.sendto(payload, udp_addr)
            except OSError:
                pass  # e.g. not on the SoftAP network yet; the stream is best-effort

    try:
        while True:
            peak = meter.GetPeakValue()  # 0.0-1.0, post-mix for the whole endpoint
            coef = attack_coef if peak > envelope else release_coef
            envelope += (peak - envelope) * coef

            db = 20 * math.log10(envelope) if envelope > 1e-9 else -120.0
            norm = min(max((db - args.floor_db) / (args.ceil_db - args.floor_db), 0.0), 1.0)
            level = round(255 * norm**args.gamma)

            payload = bytes((LEVEL_SYNC, level))
            now = time.monotonic()
            if args.depth is not None and now - last_depth_sent >= DEPTH_RESEND_S:
                payload += bytes((DEPTH_SYNC, args.depth))
                last_depth_sent = now
            send(payload)

            if ser:
                pump_serial_output(ser)
                pump_keys(ser)

            filled = round(norm * BAR_WIDTH)
            sys.stdout.write(f"\r\x1b[K[{'#' * filled}{' ' * (BAR_WIDTH - filled)}] "
                             f"{db:6.1f} dB -> {level:3d}/255")
            sys.stdout.flush()
            time.sleep(dt)
    except KeyboardInterrupt:
        pass
    finally:
        send(bytes((LEVEL_SYNC, 255)))  # restore full brightness right away
        if ser:
            ser.close()
        print("\nstopped")


if __name__ == "__main__":
    main()
