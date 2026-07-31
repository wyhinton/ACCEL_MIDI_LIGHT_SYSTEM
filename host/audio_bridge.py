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
import queue
import sys
import threading

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

# GUI status window (set in main() if enabled) and the shared stop signal.
# The BLE/audio work runs on a background thread; Tkinter owns the main
# thread. Both sides only touch _ui through its thread-safe setters.
_ui = None
_stop_event = threading.Event()


class StatusWindow:
    """Small always-on-top window showing live BLE connect status.

    Runs on the main thread; set_status()/set_level() are called from the
    background BLE/audio thread and just enqueue updates for _poll() to
    apply, so this is safe to call from either side.
    """

    COLORS = {
        "scanning":     "#e6b800",
        "connecting":   "#e6b800",
        "connected":    "#2ecc71",
        "disconnected": "#e74c3c",
        "error":        "#e74c3c",
    }

    def __init__(self, stop_event: threading.Event):
        import tkinter as tk
        from tkinter import ttk

        self._tk = tk
        self.stop_event = stop_event
        self._queue: "queue.Queue" = queue.Queue()

        self.root = tk.Tk()
        self.root.title("LightAudioBridge - BLE Status")
        self.root.geometry("360x150")
        self.root.resizable(False, False)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.canvas = tk.Canvas(self.root, width=28, height=28, highlightthickness=0)
        self.dot = self.canvas.create_oval(2, 2, 26, 26, fill=self.COLORS["scanning"], outline="")
        self.canvas.grid(row=0, column=0, padx=(16, 8), pady=(16, 4))

        self.status_var = tk.StringVar(value="Starting...")
        ttk.Label(self.root, textvariable=self.status_var, font=("Segoe UI", 11, "bold")).grid(
            row=0, column=1, sticky="w", pady=(16, 4))

        self.detail_var = tk.StringVar(value="")
        ttk.Label(self.root, textvariable=self.detail_var, wraplength=320, justify="left").grid(
            row=1, column=0, columnspan=2, sticky="w", padx=16)

        ttk.Label(self.root, text="Audio level").grid(row=2, column=0, columnspan=2,
                                                        sticky="w", padx=16, pady=(12, 0))
        self.level_bar = ttk.Progressbar(self.root, orient="horizontal", length=320,
                                          mode="determinate", maximum=100)
        self.level_bar.grid(row=3, column=0, columnspan=2, padx=16, pady=(2, 12))

        self._poll()

    def _on_close(self):
        self.stop_event.set()
        self.root.destroy()

    def _poll(self):
        try:
            while True:
                kind, payload = self._queue.get_nowait()
                if kind == "status":
                    state, text, detail = payload
                    self.canvas.itemconfig(self.dot, fill=self.COLORS.get(state, "#999999"))
                    self.status_var.set(text)
                    self.detail_var.set(detail)
                elif kind == "level":
                    self.level_bar["value"] = payload
        except queue.Empty:
            pass
        if not self.stop_event.is_set():
            self.root.after(100, self._poll)

    # -- thread-safe setters, called from the background BLE/audio thread --
    def set_status(self, state: str, text: str, detail: str = ""):
        self._queue.put(("status", (state, text, detail)))

    def set_level(self, level_0_1: float):
        self._queue.put(("level", max(0.0, min(1.0, level_0_1)) * 100))

    def run(self):
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop_event.set()


def update_level(rms: float, gain: float):
    """Fold one audio block's RMS into the shared envelope; log a meter periodically."""
    global _level, _block_count
    target = min(rms * gain, 1.0)
    coeff = ATTACK if target > _level else RELEASE
    _level += (target - _level) * coeff
    _block_count += 1
    if _ui is not None:
        _ui.set_level(_level)
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
    """Scan for the light board, retrying forever until found or _stop_event fires."""
    attempt = 0
    while not _stop_event.is_set():
        attempt += 1
        suffix = "" if attempt == 1 else f" (attempt {attempt})"
        print(f"[ble] Scanning for '{DEVICE_NAME}'{suffix} (timeout={timeout}s)...")
        if _ui is not None:
            _ui.set_status("scanning", "Searching for ESP32...",
                            f"Attempt {attempt} — looking for '{DEVICE_NAME}'")

        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)
        if dev is None:
            print(f"[ble] Name scan came up empty — falling back to service UUID scan...")
            dev = await BleakScanner.find_device_by_filter(
                lambda d, adv: SERVICE_UUID.lower() in [u.lower() for u in adv.service_uuids],
                timeout=timeout,
            )

        if dev:
            print(f"[ble] Found device: {dev.name}  address={dev.address}")
            if _ui is not None:
                _ui.set_status("connecting", "Device found, connecting...",
                                f"{dev.name or DEVICE_NAME} @ {dev.address}")
            return dev

        print(f"[ble] No device found after {timeout}s. Retrying...")
        if _ui is not None:
            _ui.set_status("disconnected", "ESP32 not found", "Retrying — is the board powered on?")

    return None


async def ble_send_loop(client):
    period = 1.0 / SEND_HZ
    while not _stop_event.is_set():
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

    stream = sd.InputStream(
        device=device,
        channels=1,
        samplerate=args.samplerate,
        blocksize=args.blocksize,
        callback=make_audio_callback(args.gain),
    )

    with stream:
        while not _stop_event.is_set():
            bridge = await find_bridge(args.scan_timeout)
            if bridge is None:
                break   # _stop_event fired mid-scan

            try:
                async with BleakClient(bridge) as client:
                    print(f"Connected to {bridge.address}. Streaming level (Ctrl+C to stop)...")
                    if _ui is not None:
                        _ui.set_status("connected", "Connected — streaming audio", bridge.address)
                    await ble_send_loop(client)
            except Exception as e:
                print(f"[ble] Connection lost: {e}", file=sys.stderr)

            if not _stop_event.is_set():
                print("[ble] Disconnected. Reconnecting...")
                if _ui is not None:
                    _ui.set_status("disconnected", "Disconnected", "Reconnecting...")
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

        try:
            while not _stop_event.is_set():
                bridge = await find_bridge(args.scan_timeout)
                if bridge is None:
                    break   # _stop_event fired mid-scan

                try:
                    async with BleakClient(bridge) as client:
                        print(f"Connected to {bridge.address}. Streaming level (Ctrl+C to stop)...")
                        if _ui is not None:
                            _ui.set_status("connected", "Connected — streaming audio", bridge.address)
                        stream.start_stream()
                        try:
                            await ble_send_loop(client)
                        finally:
                            stream.stop_stream()
                except Exception as e:
                    print(f"[ble] Connection lost: {e}", file=sys.stderr)

                if not _stop_event.is_set():
                    print("[ble] Disconnected. Reconnecting...")
                    if _ui is not None:
                        _ui.set_status("disconnected", "Disconnected", "Reconnecting...")
        finally:
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
    p.add_argument("--no-gui", action="store_true",
                   help="skip the status window; console output only")
    args = p.parse_args()

    if args.list_devices:
        if args.loopback:
            list_loopback_devices()
        else:
            list_devices()
        return

    global _ui

    if not args.no_gui:
        try:
            _ui = StatusWindow(_stop_event)
        except Exception as e:
            print(f"[gui] Could not open status window ({e}); continuing without it.",
                  file=sys.stderr)
            _ui = None

    def run_backend():
        try:
            runner = run_wasapi_loopback(args) if args.loopback else run_sounddevice(args)
            asyncio.run(runner)
        except Exception as e:
            print(f"[fatal] {e}", file=sys.stderr)
            if _ui is not None:
                _ui.set_status("error", "Fatal error", str(e))
        finally:
            _stop_event.set()

    backend_thread = threading.Thread(target=run_backend, daemon=True)
    backend_thread.start()

    if _ui is not None:
        _ui.run()               # blocks until the window is closed or _stop_event fires
        _stop_event.set()
        backend_thread.join(timeout=2)
    else:
        try:
            backend_thread.join()
        except KeyboardInterrupt:
            print("\nStopped.")
            _stop_event.set()


if __name__ == "__main__":
    main()
