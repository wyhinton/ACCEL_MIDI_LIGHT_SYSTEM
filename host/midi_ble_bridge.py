#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "bleak>=0.22",
#   "python-rtmidi>=1.5",
# ]
# ///
"""
BLE-MIDI -> virtual MIDI port bridge for the ACCEL/MIDI light system.

Connects to the SENDER board's BLE-MIDI service ("AccelLight", see
ACCELERATION_LIGHT_SENDER_REAL/src/main.cpp), decodes the BLE-MIDI
notifications it sends on jerk/crash, and forwards each MIDI message to a
named virtual MIDI port so any DAW can use it as a normal MIDI input.

Windows has no OS-level API for an application to create a new virtual MIDI
port (python-rtmidi's open_virtual_port() always raises on the Windows MM
backend), so this script instead *opens* a port by name that already exists.
Use loopMIDI (https://www.tobias-erichsen.de/software/loopmidi.html) to
create one first:

    1. Open loopMIDI, click "+" to add a new port, name it e.g. "AccelLight".
    2. Run this script; it finds any loopMIDI port whose name contains
       --port-name (default "AccelLight") and forwards messages into it.
    3. Point your DAW's MIDI input at that port.

On macOS/Linux, rtmidi can create a real virtual port directly, so this
script opens one there instead of requiring a pre-made port.

Usage:
    python midi_ble_bridge.py                       # status window (default)
    python midi_ble_bridge.py --list-ports          # show MIDI output ports and exit
    python midi_ble_bridge.py --port-name "My Port"  # match a differently-named loopMIDI port
    python midi_ble_bridge.py --no-gui               # console output only
"""

import argparse
import asyncio
import json
import os
import queue
import sys
import threading
import time

import rtmidi
from bleak import BleakClient, BleakScanner

# Must match the firmware (ACCELERATION_LIGHT_SENDER_REAL/src/main.cpp).
DEVICE_NAME = "AccelLight"
MIDI_SERVICE_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHAR_UUID = "7772e5db-3868-4112-a1a9-f2669d106bf3"
CRASH_NOTE = 36  # C1 - matches MIDI_NOTE in main.cpp, fired on jerk/crash detection

# GUI status window (set in main() if enabled) and the shared stop signal.
# BLE work runs on a background thread; Tkinter owns the main thread. Both
# sides only touch _ui through its thread-safe setters.
_ui = None
_stop_event = threading.Event()

# Guards midiout across the BLE thread (send_message) and the GUI thread
# (restart button closing/reopening the port).
_midi_lock = threading.Lock()

# Written on every status change so other processes (e.g. the Ableton launch
# script waiting for the BLE link to come up) can poll connection state
# without needing their own BLE stack.
STATUS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bridge_status.json")


def _set_status(state: str, text: str, detail: str = "") -> None:
    try:
        with open(STATUS_FILE, "w", encoding="utf-8") as f:
            json.dump({"state": state, "text": text, "detail": detail, "ts": time.time()}, f)
    except OSError as e:
        print(f"[status] Could not write {STATUS_FILE}: {e}", file=sys.stderr)
    if _ui is not None:
        _ui.set_status(state, text, detail)


def _midi_data_len(status: int) -> int:
    """Number of data bytes that follow a MIDI status byte (channel messages only)."""
    if status & 0xF0 in (0xC0, 0xD0):
        return 1
    if status in (0xF1, 0xF3):
        return 1
    if status == 0xF2:
        return 2
    if status >= 0xF6:
        return 0
    return 2


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

REALTIME_NAMES = {
    0xF8: "Timing Clock", 0xFA: "Start", 0xFB: "Continue",
    0xFC: "Stop", 0xFE: "Active Sensing", 0xFF: "Reset",
}


def note_name(n: int) -> str:
    # Matches the firmware's own convention (MIDI_NOTE=36 is commented "C1" in
    # main.cpp), i.e. middle C (60) = C3, not the C4-at-60 "scientific" scheme.
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 2}"


def describe_midi_message(msg: tuple) -> str:
    """Human-readable description of a raw MIDI message tuple, e.g. for a log/UI."""
    status = msg[0]
    hi = status & 0xF0
    ch = (status & 0x0F) + 1

    if hi == 0x80:
        _, note, vel = msg
        return f"Note Off   ch{ch}  {note_name(note):<4} ({note})  vel {vel}"
    if hi == 0x90:
        _, note, vel = msg
        kind = "Note On " if vel > 0 else "Note Off"
        return f"{kind}   ch{ch}  {note_name(note):<4} ({note})  vel {vel}"
    if hi == 0xA0:
        _, note, val = msg
        return f"Poly Pressure  ch{ch}  {note_name(note):<4} ({note})  val {val}"
    if hi == 0xB0:
        _, cc, val = msg
        return f"Control Change  ch{ch}  CC{cc} = {val}"
    if hi == 0xC0:
        _, prog = msg
        return f"Program Change  ch{ch}  #{prog}"
    if hi == 0xD0:
        _, val = msg
        return f"Channel Pressure  ch{ch}  {val}"
    if hi == 0xE0:
        _, lsb, msb = msg
        bend = ((msb << 7) | lsb) - 8192
        return f"Pitch Bend  ch{ch}  {bend:+d}"
    if status in REALTIME_NAMES:
        return REALTIME_NAMES[status]
    return f"Unknown  0x{status:02X}"


def parse_ble_midi(packet: bytes):
    """Decode a BLE-MIDI notification into a list of raw MIDI message tuples.

    Per the Bluetooth-SIG "MIDI over Bluetooth Low Energy" spec: a header
    byte, then one or more (timestamp byte)(status or running-status)(data
    bytes) groups. Sysex (0xF0) isn't handled since this firmware never
    sends it.
    """
    if len(packet) < 2:
        return []
    messages = []
    running_status = None
    i = 1  # skip the leading header byte
    n = len(packet)
    while i < n:
        if packet[i] & 0x80 and packet[i] < 0xF8:
            i += 1  # consume the per-message timestamp byte
        if i >= n:
            break
        status = packet[i]
        if status & 0x80:
            if status >= 0xF8:
                messages.append((status,))  # system realtime, no data bytes
                i += 1
                continue
            running_status = status
            i += 1
        else:
            status = running_status
            if status is None:
                break  # malformed: data byte with no prior status
        data_len = _midi_data_len(status)
        data = packet[i:i + data_len]
        if len(data) < data_len:
            break
        i += data_len
        messages.append((status, *data))
    return messages


class StatusWindow:
    """Small window showing live BLE connect status and the MIDI bridge target.

    Runs on the main thread; set_status()/set_port()/note_forwarded() are
    called from the background BLE thread and just enqueue an update for
    _poll() to apply.
    """

    COLORS = {
        "scanning":     "#e6b800",
        "connecting":   "#e6b800",
        "connected":    "#2ecc71",
        "disconnected": "#e74c3c",
        "error":        "#e74c3c",
    }

    MAX_LOG_LINES = 200

    def __init__(self, stop_event: threading.Event, on_restart=None):
        import tkinter as tk
        from tkinter import ttk

        self.stop_event = stop_event
        self.on_restart = on_restart  # set after the MIDI port is opened; see main()
        self._queue: "queue.Queue" = queue.Queue()
        self._forwarded = 0

        self.root = tk.Tk()
        self.root.title("AccelLight - MIDI Bridge")
        self.root.geometry("460x370")
        self.root.minsize(380, 270)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(5, weight=1)

        self.canvas = tk.Canvas(self.root, width=28, height=28, highlightthickness=0)
        self.dot = self.canvas.create_oval(2, 2, 26, 26, fill=self.COLORS["scanning"], outline="")
        self.canvas.grid(row=0, column=0, padx=(16, 8), pady=(16, 4))

        self.status_var = tk.StringVar(value="Starting...")
        ttk.Label(self.root, textvariable=self.status_var, font=("Segoe UI", 11, "bold")).grid(
            row=0, column=1, sticky="w", pady=(16, 4))

        self.detail_var = tk.StringVar(value="")
        ttk.Label(self.root, textvariable=self.detail_var, wraplength=420, justify="left").grid(
            row=1, column=0, columnspan=2, sticky="w", padx=16)

        self.port_var = tk.StringVar(value="MIDI port: (not open)")
        ttk.Label(self.root, textvariable=self.port_var, wraplength=420, justify="left").grid(
            row=2, column=0, columnspan=2, sticky="w", padx=16, pady=(10, 0))

        self.restart_btn = ttk.Button(
            self.root, text="Restart MIDI Port", command=self._on_restart_click)
        self.restart_btn.grid(row=3, column=0, columnspan=2, sticky="w", padx=16, pady=(6, 0))

        self.count_var = tk.StringVar(value="MIDI messages forwarded: 0")
        ttk.Label(self.root, textvariable=self.count_var).grid(
            row=4, column=0, columnspan=2, sticky="w", padx=16, pady=(4, 4))

        log_frame = ttk.Frame(self.root)
        log_frame.grid(row=5, column=0, columnspan=2, sticky="nsew", padx=16, pady=(0, 16))
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log_text = tk.Text(log_frame, height=8, wrap="none", state="disabled",
                                 font=("Consolas", 9), background="#111111", foreground="#e0e0e0")
        self.log_text.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=scrollbar.set)

        self._poll()

    def _on_close(self):
        self.stop_event.set()
        self.root.destroy()

    def _on_restart_click(self):
        if self.on_restart is None:
            return
        self.restart_btn.configure(state="disabled")
        self._append_log(f"{time.strftime('%H:%M:%S')}  Restarting MIDI port...")
        # Defer past this event so the log line above actually paints before
        # the (brief but blocking) close/reopen call runs.
        self.root.after(50, self._do_restart)

    def _do_restart(self):
        try:
            self.on_restart()
        finally:
            self.restart_btn.configure(state="normal")

    def _poll(self):
        try:
            while True:
                kind, payload = self._queue.get_nowait()
                if kind == "status":
                    state, text, detail = payload
                    self.canvas.itemconfig(self.dot, fill=self.COLORS.get(state, "#999999"))
                    self.status_var.set(text)
                    self.detail_var.set(detail)
                elif kind == "port":
                    self.port_var.set(f"MIDI port: {payload}")
                elif kind == "forwarded":
                    self._forwarded += 1
                    self.count_var.set(f"MIDI messages forwarded: {self._forwarded}")
                    self._append_log(payload)
        except queue.Empty:
            pass
        if not self.stop_event.is_set():
            self.root.after(100, self._poll)

    def _append_log(self, line: str):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line + "\n")
        # Trim from the top once the log grows past MAX_LOG_LINES.
        num_lines = int(self.log_text.index("end-1c").split(".")[0])
        if num_lines > self.MAX_LOG_LINES:
            self.log_text.delete("1.0", f"{num_lines - self.MAX_LOG_LINES}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def set_status(self, state: str, text: str, detail: str = ""):
        self._queue.put(("status", (state, text, detail)))

    def set_port(self, name: str):
        self._queue.put(("port", name))

    def note_forwarded(self, line: str):
        self._queue.put(("forwarded", line))

    def run(self):
        try:
            self.root.mainloop()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop_event.set()


def open_midi_out(port_name: str) -> rtmidi.MidiOut:
    """Open (or create, on platforms that support it) the target MIDI output port."""
    midiout = rtmidi.MidiOut()
    ports = midiout.get_ports()
    match = next((i for i, p in enumerate(ports) if port_name.lower() in p.lower()), None)

    if match is not None:
        midiout.open_port(match)
        print(f"[midi] Opened port: {ports[match]}")
        if _ui is not None:
            _ui.set_port(ports[match])
        return midiout

    try:
        midiout.open_virtual_port(port_name)
        print(f"[midi] Created virtual port: {port_name}")
        if _ui is not None:
            _ui.set_port(port_name)
        return midiout
    except (NotImplementedError, RuntimeError) as e:
        available = "\n  ".join(ports) if ports else "(none)"
        print(
            f"[midi] No port matching '{port_name}' found, and this platform can't "
            f"create one ({e}).\n"
            f"On Windows, create one first with loopMIDI "
            f"(https://www.tobias-erichsen.de/software/loopmidi.html), name it "
            f"'{port_name}', then re-run this script.\n"
            f"Available output ports:\n  {available}",
            file=sys.stderr,
        )
        raise SystemExit(1)


def restart_midi_port(midiout: rtmidi.MidiOut, port_name: str) -> None:
    """Close and reopen the MIDI output port.

    Ableton only scans MIDI devices at launch (or when the MIDI preferences
    pane reopens), so a loopMIDI port that appears/changes after that stays
    invisible to it even though other apps (which poll more eagerly) see it
    fine. Toggling the port off and on re-announces it at the OS level,
    which is often enough to get Ableton to pick it up on its next rescan
    without restarting Ableton itself.
    """
    with _midi_lock:
        print("[midi] Restarting MIDI port...")
        try:
            midiout.close_port()
        except Exception as e:
            print(f"[midi] close_port failed: {e}", file=sys.stderr)
        time.sleep(0.3)

        ports = midiout.get_ports()
        match = next((i for i, p in enumerate(ports) if port_name.lower() in p.lower()), None)
        try:
            if match is not None:
                midiout.open_port(match)
                print(f"[midi] Reopened port: {ports[match]}")
                if _ui is not None:
                    _ui.set_port(ports[match])
            else:
                midiout.open_virtual_port(port_name)
                print(f"[midi] Recreated virtual port: {port_name}")
                if _ui is not None:
                    _ui.set_port(port_name)
        except (NotImplementedError, RuntimeError) as e:
            print(f"[midi] Failed to reopen MIDI port: {e}", file=sys.stderr)
            if _ui is not None:
                _ui.set_port(f"(error reopening: {e})")


def midi_notification_handler(midiout: rtmidi.MidiOut):
    def handler(_sender, data: bytearray):
        print(f"[ble] Notification received ({len(data)} bytes)")
        for msg in parse_ble_midi(bytes(data)):
            with _midi_lock:
                midiout.send_message(msg)
            msg_hex = " ".join(f"{b:02X}" for b in msg)
            line = f"{time.strftime('%H:%M:%S')}  {describe_midi_message(msg):<34} [{msg_hex}]"
            print(f"[midi] {line}")
            if _ui is not None:
                _ui.note_forwarded(line)

            if len(msg) == 3 and (msg[0] & 0xF0) == 0x90 and msg[1] == CRASH_NOTE and msg[2] > 0:
                print(f"[crash] CRASH DETECTED at {time.strftime('%H:%M:%S')} (velocity {msg[2]})")
                _set_status("connected", "Crash detected!", f"note {CRASH_NOTE} vel {msg[2]}")
    return handler


async def find_device(timeout: float):
    """Scan for the light board, retrying forever until found or _stop_event fires."""
    attempt = 0
    while not _stop_event.is_set():
        attempt += 1
        suffix = "" if attempt == 1 else f" (attempt {attempt})"
        print(f"[ble] Scanning for '{DEVICE_NAME}'{suffix} (timeout={timeout}s)...")
        _set_status("scanning", "Searching for ESP32...",
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
            _set_status("connecting", "Device found, connecting...",
                         f"{dev.name or DEVICE_NAME} @ {dev.address}")
            return dev

        print(f"[ble] No device found after {timeout}s. Retrying...")
        _set_status("disconnected", "ESP32 not found", "Retrying — is the board powered on?")

    return None


async def watch_connection(args, midiout: rtmidi.MidiOut):
    """Connect, forward MIDI notifications while the link holds, and retry forever."""
    handler = midi_notification_handler(midiout)

    while not _stop_event.is_set():
        dev = await find_device(args.scan_timeout)
        if dev is None:
            break  # _stop_event fired mid-scan

        try:
            async with BleakClient(dev) as client:
                await client.start_notify(MIDI_CHAR_UUID, handler)
                print(f"Connected to {dev.address}. Forwarding MIDI to '{args.port_name}'.")
                _set_status("connected", "Connected", dev.address)
                while client.is_connected and not _stop_event.is_set():
                    await asyncio.sleep(0.5)
        except Exception as e:
            print(f"[ble] Connection lost: {e}", file=sys.stderr)

        if not _stop_event.is_set():
            print("[ble] Disconnected. Reconnecting...")
            _set_status("disconnected", "Disconnected", "Reconnecting...")
    return 0


def main():
    p = argparse.ArgumentParser(description="BLE-MIDI -> virtual MIDI port bridge")
    p.add_argument("--scan-timeout", type=float, default=10.0)
    p.add_argument("--port-name", default=DEVICE_NAME,
                   help="name (or substring) of the MIDI output port to use/create")
    p.add_argument("--list-ports", action="store_true",
                   help="list available MIDI output ports and exit")
    p.add_argument("--no-gui", action="store_true",
                   help="skip the status window; console output only")
    args = p.parse_args()

    if args.list_ports:
        for name in rtmidi.MidiOut().get_ports():
            print(name)
        return

    _set_status("starting", "Bridge starting...", "")

    global _ui

    if not args.no_gui:
        try:
            _ui = StatusWindow(_stop_event)
        except Exception as e:
            print(f"[gui] Could not open status window ({e}); continuing without it.",
                  file=sys.stderr)
            _ui = None

    midiout = open_midi_out(args.port_name)

    if _ui is not None:
        _ui.on_restart = lambda: restart_midi_port(midiout, args.port_name)

    def run_backend():
        try:
            asyncio.run(watch_connection(args, midiout))
        except Exception as e:
            print(f"[fatal] {e}", file=sys.stderr)
            _set_status("error", "Fatal error", str(e))
        finally:
            _stop_event.set()

    backend_thread = threading.Thread(target=run_backend, daemon=True)
    backend_thread.start()

    if _ui is not None:
        _ui.run()  # blocks until the window is closed or _stop_event fires
        _stop_event.set()
        backend_thread.join(timeout=2)
    else:
        try:
            backend_thread.join()
        except KeyboardInterrupt:
            print("\nStopped.")
            _stop_event.set()

    midiout.close_port()


if __name__ == "__main__":
    main()
