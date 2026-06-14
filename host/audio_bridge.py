#!/usr/bin/env python3
"""
Audio -> BLE bridge for the ACCEL/MIDI light system.

Captures the level of an audio input device, smooths it into an envelope, and
streams it (one byte, 0..255) over BLE to the MIDI board ("LightAudioBridge"),
which rebroadcasts it over ESP-NOW so the light boards use it as a brightness
multiplier.

macOS note: there's no clean way to grab *system output* audio from Python, so
route your output through a virtual loopback device (BlackHole) and capture that
device here. Use a macOS "Multi-Output Device" so you still hear the audio.
On Windows you can instead pick "Stereo Mix" or a WASAPI loopback input.

Usage:
    python audio_bridge.py --list-devices         # see input device indexes
    python audio_bridge.py --device "BlackHole"   # capture that device, stream BLE
    python audio_bridge.py --device 3 --gain 4.0  # by index, with extra gain

Dependencies: pip install -r requirements.txt
"""

import argparse
import asyncio
import sys

import numpy as np
import sounddevice as sd
from bleak import BleakClient, BleakScanner

# Must match the firmware (MIDI_NOTE_LIGHT_REAL/src/main.cpp).
DEVICE_NAME = "LightAudioBridge"
SERVICE_UUID = "9a0b0000-1234-4c6e-9b00-1f2e3d4c5b6a"
CHAR_UUID = "9a0b0001-1234-4c6e-9b00-1f2e3d4c5b6a"

# Envelope smoothing per audio block. Fast attack so peaks pop, slower release
# so it eases back down — this is the "smooth average."
ATTACK = 0.5    # 0..1, higher = snappier rise
RELEASE = 0.05  # 0..1, lower = slower fall

SEND_HZ = 40    # BLE writes per second

# Shared envelope level (0.0..1.0), written by the audio thread, read by asyncio.
_level = 0.0


def make_audio_callback(gain: float):
    def callback(indata, frames, time_info, status):
        global _level
        if status:
            print(status, file=sys.stderr)
        # RMS of this block across all channels, scaled by gain and clamped.
        rms = float(np.sqrt(np.mean(np.square(indata, dtype=np.float64))))
        target = min(rms * gain, 1.0)
        coeff = ATTACK if target > _level else RELEASE
        _level += (target - _level) * coeff
    return callback


def list_devices():
    print(sd.query_devices())


async def find_bridge(timeout: float):
    print(f"Scanning for '{DEVICE_NAME}'...")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)
    if dev is None:
        # Fall back to matching by advertised service UUID.
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: SERVICE_UUID.lower() in [u.lower() for u in adv.service_uuids],
            timeout=timeout,
        )
    return dev


async def run(args):
    device = find_input_device(args.device)
    print(f"Capturing audio from: {device}")

    bridge = await find_bridge(args.scan_timeout)
    if bridge is None:
        print(f"Could not find BLE device '{DEVICE_NAME}'. Is the MIDI board powered?",
              file=sys.stderr)
        return 1

    stream = sd.InputStream(
        device=device,
        channels=1,
        samplerate=args.samplerate,
        blocksize=args.blocksize,
        callback=make_audio_callback(args.gain),
    )

    async with BleakClient(bridge) as client:
        print(f"Connected to {bridge.address}. Streaming level (Ctrl+C to stop)...")
        with stream:
            period = 1.0 / SEND_HZ
            while True:
                level_byte = int(round(min(max(_level, 0.0), 1.0) * 255))
                try:
                    await client.write_gatt_char(CHAR_UUID, bytes([level_byte]),
                                                 response=False)
                except Exception as e:
                    print(f"BLE write failed: {e}", file=sys.stderr)
                    break
                await asyncio.sleep(period)
    return 0


def find_input_device(spec):
    """Resolve a device index or a (case-insensitive) name substring."""
    if spec is None:
        return None  # sounddevice default input
    try:
        return int(spec)
    except ValueError:
        pass
    for idx, dev in enumerate(sd.query_devices()):
        if dev["max_input_channels"] > 0 and spec.lower() in dev["name"].lower():
            return idx
    raise SystemExit(f"No input device matching '{spec}'. Try --list-devices.")


def main():
    p = argparse.ArgumentParser(description="Audio -> BLE level bridge")
    p.add_argument("--device", help="input device index or name substring "
                                     "(e.g. BlackHole). Default: system default input")
    p.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    p.add_argument("--gain", type=float, default=4.0, help="multiply RMS before clamping")
    p.add_argument("--samplerate", type=int, default=48000)
    p.add_argument("--blocksize", type=int, default=512)
    p.add_argument("--scan-timeout", type=float, default=10.0)
    args = p.parse_args()

    if args.list_devices:
        list_devices()
        return

    try:
        sys.exit(asyncio.run(run(args)))
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
