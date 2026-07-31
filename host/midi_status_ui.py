#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "bleak>=0.22",
# ]
# ///
"""
BLE-MIDI connection status UI for the ACCEL/MIDI light system.

Connects to the SENDER board's BLE-MIDI service ("AccelLight", see
ACCELERATION_LIGHT_SENDER_REAL/src/main.cpp) purely to show live connection
status in a small window: scanning, connecting, connected, disconnected —
retrying forever if the board isn't found on startup or the link drops later.

This does NOT decode or relay MIDI notes anywhere. A standard BLE-MIDI
peripheral is picked up natively by the OS's own Bluetooth MIDI stack
(e.g. Windows Bluetooth LE MIDI) once paired, and shows up directly as a MIDI
input in any DAW — no companion app is needed for that part. This script's
own BLE connection is separate and only drives the status window (and, on
the firmware side, the matrix's status pixel + connect flash).

Usage:
    python midi_status_ui.py                 # status window (default)
    python midi_status_ui.py --no-gui         # console output only
"""

import argparse
import asyncio
import queue
import sys
import threading

from bleak import BleakClient, BleakScanner

# Must match the firmware (ACCELERATION_LIGHT_SENDER_REAL/src/main.cpp).
DEVICE_NAME = "AccelLight"
MIDI_SERVICE_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"

# GUI status window (set in main() if enabled) and the shared stop signal.
# BLE work runs on a background thread; Tkinter owns the main thread. Both
# sides only touch _ui through its thread-safe setters.
_ui = None
_stop_event = threading.Event()


class StatusWindow:
    """Small window showing live BLE connect status.

    Runs on the main thread; set_status() is called from the background BLE
    thread and just enqueues an update for _poll() to apply.
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

        self.stop_event = stop_event
        self._queue: "queue.Queue" = queue.Queue()

        self.root = tk.Tk()
        self.root.title("AccelLight - BLE-MIDI Status")
        self.root.geometry("360x120")
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
            row=1, column=0, columnspan=2, sticky="w", padx=16, pady=(0, 16))

        self._poll()

    def _on_close(self):
        self.stop_event.set()
        self.root.destroy()

    def _poll(self):
        try:
            while True:
                state, text, detail = self._queue.get_nowait()
                self.canvas.itemconfig(self.dot, fill=self.COLORS.get(state, "#999999"))
                self.status_var.set(text)
                self.detail_var.set(detail)
        except queue.Empty:
            pass
        if not self.stop_event.is_set():
            self.root.after(100, self._poll)

    def set_status(self, state: str, text: str, detail: str = ""):
        self._queue.put((state, text, detail))

    def run(self):
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop_event.set()


async def find_device(timeout: float):
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
            print("[ble] Name scan came up empty — falling back to service UUID scan...")
            dev = await BleakScanner.find_device_by_filter(
                lambda d, adv: MIDI_SERVICE_UUID in [u.lower() for u in adv.service_uuids],
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


async def watch_connection(args):
    """Connect, hold the connection open while polling for a drop, and retry forever."""
    while not _stop_event.is_set():
        dev = await find_device(args.scan_timeout)
        if dev is None:
            break   # _stop_event fired mid-scan

        try:
            async with BleakClient(dev) as client:
                print(f"Connected to {dev.address}.")
                if _ui is not None:
                    _ui.set_status("connected", "Connected", dev.address)
                while client.is_connected and not _stop_event.is_set():
                    await asyncio.sleep(0.5)
        except Exception as e:
            print(f"[ble] Connection lost: {e}", file=sys.stderr)

        if not _stop_event.is_set():
            print("[ble] Disconnected. Reconnecting...")
            if _ui is not None:
                _ui.set_status("disconnected", "Disconnected", "Reconnecting...")
    return 0


def main():
    p = argparse.ArgumentParser(description="BLE-MIDI connection status UI")
    p.add_argument("--scan-timeout", type=float, default=10.0)
    p.add_argument("--no-gui", action="store_true",
                   help="skip the status window; console output only")
    args = p.parse_args()

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
            asyncio.run(watch_connection(args))
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
