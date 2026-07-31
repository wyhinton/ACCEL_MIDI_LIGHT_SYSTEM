#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "bleak>=0.22",
#   "sounddevice>=0.4.6",
#   "numpy>=1.24",
#   "pyaudiowpatch>=0.2.12.7; sys_platform == 'win32'",
# ]
# ///
"""
Audio -> BLE bridge for the ACCEL/MIDI light system.

Captures the level of an audio input device, smooths it into an envelope, and
streams it (one byte, 0..255) over BLE directly to the SENDER light board
("LightAudioBridge"), which uses it as a brightness multiplier on its pulse.

macOS note: there's no clean way to grab *system output* audio from Python, so
route your output through a virtual loopback device (BlackHole) and capture that
device here. Use a macOS "Multi-Output Device" so you still hear the audio.

Windows note: use --loopback to capture system output audio directly via WASAPI
loopback — no Stereo Mix / virtual cable needed (and most modern drivers don't
expose Stereo Mix anyway). Requires `pyaudiowpatch` (installed automatically by
`uv run` on Windows; otherwise `pip install pyaudiowpatch`).

Usage:
    python audio_bridge.py --list-devices          # see input device indexes (mac/linux)
    python audio_bridge.py --device "BlackHole"    # capture that device, stream BLE

    python audio_bridge.py --loopback                        # Windows: default speakers
    python audio_bridge.py --loopback --device "Headphones"  # Windows: specific output
    python audio_bridge.py --list-devices --loopback         # Windows: list loopback devices

Dependencies: pip install -r requirements.txt
"""

import argparse
import asyncio
import sys

import numpy as np
import sounddevice as sd
from bleak import BleakClient, BleakScanner

# Must match the firmware (ACCELERATION_LIGHT_SENDER_REAL/src/main.cpp).
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
_block_count = 0


def update_level(rms: float, gain: float):
    """Fold one audio block's RMS into the shared envelope; log a meter periodically."""
    global _level, _block_count
    target = min(rms * gain, 1.0)
    coeff = ATTACK if target > _level else RELEASE
    _level += (target - _level) * coeff
    _block_count += 1
    if _block_count % 100 == 0:
        bar = "#" * int(_level * 40)
        print(f"[audio] rms={rms:.4f}  gain={gain:.1f}  target={target:.3f}  "
              f"level={_level:.3f}  [{bar:<40}]")


def make_audio_callback(gain: float):
    """sounddevice InputStream callback (float32 samples in -1..1)."""
    def callback(indata, frames, time_info, status):
        if status:
            print(f"[audio] stream status: {status}", file=sys.stderr)
        rms = float(np.sqrt(np.mean(np.square(indata, dtype=np.float64))))
        update_level(rms, gain)
    return callback


def make_pyaudio_callback(gain: float):
    """PyAudioWPatch stream callback (raw 16-bit PCM bytes) for WASAPI loopback."""
    import pyaudiowpatch as pyaudio

    def callback(in_data, frame_count, time_info, status):
        samples = np.frombuffer(in_data, dtype=np.int16).astype(np.float64) / 32768.0
        rms = float(np.sqrt(np.mean(np.square(samples)))) if samples.size else 0.0
        update_level(rms, gain)
        return (None, pyaudio.paContinue)
    return callback


def list_devices():
    print("\nAvailable audio devices:")
    print("-" * 60)
    for idx, dev in enumerate(sd.query_devices()):
        direction = []
        if dev["max_input_channels"] > 0:
            direction.append(f"in:{dev['max_input_channels']}ch")
        if dev["max_output_channels"] > 0:
            direction.append(f"out:{dev['max_output_channels']}ch")
        print(f"  [{idx:2d}] {dev['name']:<40} {' '.join(direction)}")
    print("-" * 60)


def list_loopback_devices():
    try:
        import pyaudiowpatch as pyaudio
    except ImportError:
        raise SystemExit("--loopback needs pyaudiowpatch (Windows only): "
                          "pip install pyaudiowpatch")
    p = pyaudio.PyAudio()
    try:
        print("\nWASAPI loopback-capable devices (use with --loopback --device):")
        print("-" * 60)
        for dev in p.get_loopback_device_info_generator():
            print(f"  [{dev['index']:2d}] {dev['name']}")
        print("-" * 60)
    finally:
        p.terminate()


async def find_bridge(timeout: float):
    print(f"[ble] Scanning for '{DEVICE_NAME}' (timeout={timeout}s)...")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)
    if dev is None:
        print(f"[ble] Name scan came up empty — falling back to service UUID scan...")
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: SERVICE_UUID.lower() in [u.lower() for u in adv.service_uuids],
            timeout=timeout,
        )
    if dev:
        print(f"[ble] Found device: {dev.name}  address={dev.address}")
    else:
        print(f"[ble] No device found after {timeout}s.")
    return dev


async def ble_send_loop(client):
    period = 1.0 / SEND_HZ
    while True:
        level_byte = int(round(min(max(_level, 0.0), 1.0) * 255))
        try:
            await client.write_gatt_char(CHAR_UUID, bytes([level_byte]), response=False)
        except Exception as e:
            print(f"BLE write failed: {e}", file=sys.stderr)
            break
        await asyncio.sleep(period)


async def run_sounddevice(args):
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
            await ble_send_loop(client)
    return 0


async def run_wasapi_loopback(args):
    try:
        import pyaudiowpatch as pyaudio
    except ImportError:
        print("--loopback needs pyaudiowpatch (Windows only): pip install pyaudiowpatch",
              file=sys.stderr)
        return 1

    p = pyaudio.PyAudio()
    try:
        try:
            wasapi_info = p.get_host_api_info_by_type(pyaudio.paWASAPI)
        except OSError:
            print("WASAPI is not available on this system (--loopback is Windows-only).",
                  file=sys.stderr)
            return 1

        device = None
        if args.device:
            try:
                device = p.get_device_info_by_index(int(args.device))
            except ValueError:
                for loopback in p.get_loopback_device_info_generator():
                    if args.device.lower() in loopback["name"].lower():
                        device = loopback
                        break
            if device is None:
                print(f"No WASAPI loopback device matching '{args.device}'. "
                      f"Try --list-devices --loopback.", file=sys.stderr)
                return 1
        else:
            default_speakers = p.get_device_info_by_index(wasapi_info["defaultOutputDevice"])
            if default_speakers.get("isLoopbackDevice"):
                device = default_speakers
            else:
                for loopback in p.get_loopback_device_info_generator():
                    if default_speakers["name"] in loopback["name"]:
                        device = loopback
                        break
            if device is None:
                print("Could not find the loopback counterpart of the default output "
                      f"device ('{default_speakers['name']}'). Try --list-devices --loopback.",
                      file=sys.stderr)
                return 1

        print(f"Capturing system audio (WASAPI loopback) from: {device['name']}")

        bridge = await find_bridge(args.scan_timeout)
        if bridge is None:
            print(f"Could not find BLE device '{DEVICE_NAME}'. Is the MIDI board powered?",
                  file=sys.stderr)
            return 1

        stream = p.open(
            format=pyaudio.paInt16,
            channels=int(device["maxInputChannels"]),
            rate=int(device["defaultSampleRate"]),
            frames_per_buffer=args.blocksize,
            input=True,
            input_device_index=device["index"],
            start=False,
            stream_callback=make_pyaudio_callback(args.gain),
        )

        async with BleakClient(bridge) as client:
            print(f"Connected to {bridge.address}. Streaming level (Ctrl+C to stop)...")
            stream.start_stream()
            try:
                await ble_send_loop(client)
            finally:
                stream.stop_stream()
                stream.close()
    finally:
        p.terminate()
    return 0


def find_input_device(spec):
    """Resolve a plain input device index or a (case-insensitive) name substring."""
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
                                     "(e.g. BlackHole). With --loopback, selects the "
                                     "output device to loop back instead. "
                                     "Default: system default input/output")
    p.add_argument("--loopback", action="store_true",
                   help="Windows only: capture system output audio directly via WASAPI "
                        "loopback (needs pyaudiowpatch) instead of reading an input device")
    p.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    p.add_argument("--gain", type=float, default=4.0, help="multiply RMS before clamping")
    p.add_argument("--samplerate", type=int, default=48000,
                   help="capture sample rate (ignored with --loopback, which uses the "
                        "device's own rate)")
    p.add_argument("--blocksize", type=int, default=512)
    p.add_argument("--scan-timeout", type=float, default=10.0)
    args = p.parse_args()

    if args.list_devices:
        if args.loopback:
            list_loopback_devices()
        else:
            list_devices()
        return

    try:
        runner = run_wasapi_loopback(args) if args.loopback else run_sounddevice(args)
        sys.exit(asyncio.run(runner))
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
