#!/usr/bin/env python3
"""
MIDI Note Recorder UI  (branch: midi_rec_for_recording)
=======================================================

A tkinter front-end for the ESP32 "midi-recorder-bridge" firmware running on
MIDI_NOTE_LIGHT_REAL. The board forwards hardware DIN MIDI it hears to USB as
simple ASCII lines, and accepts playback commands back. This app:

  * connects to the board over USB serial (auto-lists ports),
  * shows a live connection status + heartbeat (in/out/err counters),
  * shows incoming notes as they arrive (event monitor + active-note display),
  * RECORDS incoming events with a host-side clock,
  * PLAYS BACK a recording either by streaming timed commands to the board
    (emitted out the FeatherWing MIDI OUT jack) or, if python-rtmidi is
    installed, straight to a system MIDI output port (e.g. an
    iConnectMIDI2+), picked in the "Playback Output" panel,
  * SAVES / LOADS recordings as JSON, and exports a Standard MIDI File (.mid),
  * has a raw console + debug toggles (PING test, show heartbeats, show raw,
    send manual commands) so you can see exactly what the link is doing.

Dependencies:  pyserial   (pip install pyserial)
               python-rtmidi (optional; pip install python-rtmidi) for the
               direct-to-system-MIDI-output playback path.
tkinter ships with CPython on Windows/macOS and most Linux python3 packages.

Protocol (must match MIDI_NOTE_LIGHT_REAL/src/main.cpp):
  Board -> PC : READY ... | #info | EVT <ms> <TYPE> <ch> <d1> <d2> | STAT ... | PONG ... | ECHO ...
  PC -> Board : PING | ECHO x | STAT | PNON/PNOF/PCC/PPB/PPC | ALLOFF [ch] | RAW <hex...>
"""

import json
import os
import queue
import re
import socket
import struct
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - import guard for a friendlier message
    raise SystemExit(
        "pyserial is required.  Install it with:  pip install pyserial"
    )

try:
    import rtmidi  # python-rtmidi: lets playback go straight to a system MIDI
                    # output (e.g. an iConnectMIDI2+) instead of the ESP32.
except ImportError:
    rtmidi = None

BAUD = 115200
# SoftAP defaults baked into the firmware (MIDI_NOTE_LIGHT_REAL/src/main.cpp).
WIFI_SSID = "MIDI-Recorder"
WIFI_PASS = "midi1234"
WIFI_HOST = "192.168.4.1"   # board's SoftAP IP
WIFI_PORT = 5000
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# MIDI event types the board can emit (EVT lines), in display order.
# Only Program Change is visible/recorded by default; the rest must be
# opted into via the "Event Filters" panel.
EVENT_TYPES = [
    ("NON", "Note On"),
    ("NOF", "Note Off"),
    ("CC", "Ctrl Change"),
    ("PC", "Prog Change"),
    ("PB", "Pitch Bend"),
    ("AT", "Aftertouch"),
    ("CAT", "Chan Pressure"),
]
DEFAULT_VISIBLE_TYPES = {"PC"}

# Named recordings ("songs") are persisted as one JSON file per song here.
SONGS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "songs")

# Playback destination options: the ESP32's hardware MIDI OUT jack, or (if
# python-rtmidi is installed) a real system MIDI output port picked by name.
ESP32_OUTPUT_LABEL = "ESP32 (board MIDI OUT)"


def note_name(note: int) -> str:
    """MIDI note number -> name like 'C4' (60 = C4, middle C convention)."""
    return f"{NOTE_NAMES[note % 12]}{note // 12 - 1}"


# =====================================================================
#  Serial link: a background reader thread + a write helper.
# =====================================================================
class SerialLink:
    """Owns the pyserial port. Reads lines on a thread, pushes them to a queue."""

    def __init__(self, line_queue: "queue.Queue"):
        self.line_queue = line_queue
        self.ser: "serial.Serial | None" = None
        self._reader: "threading.Thread | None" = None
        self._stop = threading.Event()

    @property
    def is_open(self) -> bool:
        return self.ser is not None and self.ser.is_open

    def open(self, port: str, baud: int = BAUD):
        self.close()
        # Modest read timeout so the reader thread can notice _stop promptly.
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def close(self):
        self._stop.set()
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=1.0)
        self._reader = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def _read_loop(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
            except Exception as exc:
                self.line_queue.put(("error", f"serial read failed: {exc}"))
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip("\r\n")
                if text:
                    self.line_queue.put(("line", text))
        self.line_queue.put(("closed", ""))

    def write_line(self, text: str) -> bool:
        if not self.is_open:
            return False
        try:
            self.ser.write((text + "\n").encode("ascii", "ignore"))
            return True
        except Exception as exc:
            self.line_queue.put(("error", f"serial write failed: {exc}"))
            return False


class TcpLink:
    """WiFi transport: a TCP socket to the board's SoftAP. Same interface as
    SerialLink (open/close/write_line/is_open + a background line reader) so the
    app is transport-agnostic."""

    def __init__(self, line_queue: "queue.Queue"):
        self.line_queue = line_queue
        self.sock: "socket.socket | None" = None
        self._reader: "threading.Thread | None" = None
        self._stop = threading.Event()

    @property
    def is_open(self) -> bool:
        return self.sock is not None

    def open(self, host: str, port: int):
        self.close()
        sock = socket.create_connection((host, port), timeout=5.0)
        sock.settimeout(0.2)            # so the reader can poll _stop
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = sock
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def close(self):
        self._stop.set()
        if self._reader and self._reader.is_alive():
            self._reader.join(timeout=1.0)
        self._reader = None
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None

    def _read_loop(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self.sock.recv(512)
            except socket.timeout:
                continue
            except Exception as exc:
                self.line_queue.put(("error", f"tcp read failed: {exc}"))
                break
            if not chunk:                # peer closed
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip("\r\n")
                if text:
                    self.line_queue.put(("line", text))
        self.line_queue.put(("closed", ""))

    def write_line(self, text: str) -> bool:
        if not self.is_open:
            return False
        try:
            self.sock.sendall((text + "\n").encode("ascii", "ignore"))
            return True
        except Exception as exc:
            self.line_queue.put(("error", f"tcp write failed: {exc}"))
            return False


# =====================================================================
#  Raw MIDI byte encoding, shared by the SMF exporter and live playback
#  (both ESP32-command and direct-system-MIDI-output paths).
# =====================================================================
def midi_bytes(etype: str, ch: int, d1: int, d2: int) -> "bytes | None":
    """Encode one recorded event as status+data bytes (channel 1..16 -> 0..15).
    Returns None for event types with no raw-MIDI equivalent."""
    c = max(0, min(15, int(ch) - 1))
    d1, d2 = int(d1), int(d2)
    if etype == "NON":
        return bytes([0x90 | c, d1 & 0x7F, d2 & 0x7F])
    if etype == "NOF":
        return bytes([0x80 | c, d1 & 0x7F, d2 & 0x7F])
    if etype == "CC":
        return bytes([0xB0 | c, d1 & 0x7F, d2 & 0x7F])
    if etype == "PC":
        return bytes([0xC0 | c, d1 & 0x7F])
    if etype == "PB":
        val = max(0, min(16383, d1))
        return bytes([0xE0 | c, val & 0x7F, (val >> 7) & 0x7F])
    if etype == "CAT":
        return bytes([0xD0 | c, d1 & 0x7F])
    if etype == "AT":
        return bytes([0xA0 | c, d1 & 0x7F, d2 & 0x7F])
    return None


# =====================================================================
#  Standard MIDI File export (no external deps).
# =====================================================================
def _vlq(value: int) -> bytes:
    """Encode an int as a MIDI variable-length quantity."""
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def export_smf(path: str, events: list, ppq: int = 480, bpm: float = 120.0):
    """Write recorded events (each {'t': seconds, 'type', 'ch', 'd1', 'd2'}) to a
    type-0 Standard MIDI File. Channel/note/CC/PB/PC are supported."""
    us_per_quarter = int(60_000_000 / bpm)
    seconds_per_tick = (us_per_quarter / 1_000_000) / ppq

    track = bytearray()
    # Tempo meta event at t=0.
    track += _vlq(0) + b"\xFF\x51\x03" + us_per_quarter.to_bytes(3, "big")

    last_tick = 0
    for ev in sorted(events, key=lambda e: e["t"]):
        tick = int(round(ev["t"] / seconds_per_tick))
        delta = max(0, tick - last_tick)
        last_tick = tick
        msg = midi_bytes(ev["type"], ev["ch"], ev["d1"], ev["d2"])
        if msg is None:
            continue
        track += _vlq(delta) + msg

    track += _vlq(0) + b"\xFF\x2F\x00"  # end of track

    with open(path, "wb") as fh:
        fh.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppq))
        fh.write(b"MTrk" + struct.pack(">I", len(track)) + bytes(track))


# =====================================================================
#  Timeline: a horizontal strip showing where Program Change events land
#  in the current take, plus a playhead that tracks recording/playback.
# =====================================================================
class TimelineView:
    MARGIN = 10

    def __init__(self, parent, height: int = 90):
        self.canvas = tk.Canvas(parent, height=height, background="#181c22",
                                 highlightthickness=0)
        self.events: list = []
        self.duration = 1.0
        self.playhead_t = 0.0
        self.canvas.bind("<Configure>", lambda e: self._redraw_static())

    def set_events(self, events: list):
        self.events = events
        self.duration = max(1.0, max((e["t"] for e in events), default=0.0))
        self._redraw_static()

    def set_playhead(self, t: float):
        self.playhead_t = max(0.0, t)
        if self.playhead_t > self.duration:
            self.duration = self.playhead_t
            self._redraw_static()
        else:
            self._redraw_playhead()

    def _x_of(self, t: float) -> float:
        w = max(1, self.canvas.winfo_width() - 2 * self.MARGIN)
        return self.MARGIN + (t / self.duration) * w

    def _redraw_static(self):
        c = self.canvas
        c.delete("static")
        w, h = c.winfo_width(), c.winfo_height()
        if w <= 1:
            return
        baseline_y = h - 22
        c.create_line(self.MARGIN, baseline_y, w - self.MARGIN, baseline_y,
                      fill="#3a4250", tags="static")
        step = self._nice_step(self.duration)
        t = 0.0
        while t <= self.duration + 1e-9:
            x = self._x_of(t)
            c.create_line(x, baseline_y - 4, x, baseline_y + 4, fill="#3a4250", tags="static")
            c.create_text(x, baseline_y + 12, text=f"{t:g}s", fill="#7b8494",
                          font=("Consolas", 8), tags="static")
            t += step
        for ev in self.events:
            if ev.get("type") != "PC":
                continue
            x = self._x_of(ev["t"])
            c.create_line(x, 8, x, baseline_y, fill="#ffcb6b", tags="static")
            c.create_text(x, 6, text=str(ev["d1"]), fill="#ffcb6b", anchor="n",
                          font=("Consolas", 9, "bold"), tags="static")
        self._redraw_playhead()

    def _redraw_playhead(self):
        c = self.canvas
        c.delete("playhead")
        h = c.winfo_height()
        x = self._x_of(self.playhead_t)
        c.create_line(x, 0, x, h, fill="#ff6b6b", width=2, tags="playhead")

    @staticmethod
    def _nice_step(duration: float) -> float:
        raw = duration / 8
        for step in (0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300):
            if raw <= step:
                return step
        return 600.0


# =====================================================================
#  The application.
# =====================================================================
class RecorderApp:
    POLL_MS = 25  # how often the Tk loop drains the serial queue

    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("ESP32 MIDI Recorder")
        root.geometry("960x680")
        root.minsize(820, 560)

        self.line_queue: "queue.Queue" = queue.Queue()
        self.serial_link = SerialLink(self.line_queue)
        self.tcp_link = TcpLink(self.line_queue)
        self.transport = tk.StringVar(value="USB Serial")
        self.link = self.serial_link   # active transport; reassigned on connect

        # recording state
        self.recording = False
        self.rec_start_ms: "int | None" = None  # board ms of first recorded event
        self.events: list = []          # captured events (current take)
        self.active_notes: dict = {}    # (ch, note) -> velocity, for live display

        # song state: a "song" is a named take persisted to SONGS_DIR. Record
        # writes into whichever song is currently selected.
        self.current_song: "str | None" = None

        # playback state
        self.playing = False
        self._play_thread: "threading.Thread | None" = None
        self._play_stop = threading.Event()
        self._play_t0: "float | None" = None    # time.monotonic() at playback start
        self._play_speed = 1.0                  # speed captured at playback start
        self._play_dest = ESP32_OUTPUT_LABEL    # plain-str snapshot of playback_dest
                                                 # (the _play_loop thread must never
                                                 # touch a Tk Var directly)

        # playback destination: the ESP32's own MIDI OUT jack, or (via
        # python-rtmidi) a real system MIDI output port, e.g. an
        # iConnectMIDI2+. Independent of the ESP32 connection used for
        # recording / the MIDI OUT wiring test below.
        self.playback_dest = tk.StringVar(value=ESP32_OUTPUT_LABEL)
        self.midiout = None   # open rtmidi.MidiOut, or None when using the ESP32

        # multi-track record: replays the current song for a set number of
        # passes, prompting between passes so hardware can be reconfigured.
        self.multitrack_active = False
        self.multitrack_pass = 0
        self.multitrack_total = 4

        # MIDI-out test toggle: pulses a fixed note (ch1 C4) to confirm the
        # hardware MIDI OUT wiring is working, independent of any recording.
        self.testing_midi = False
        self._test_thread: "threading.Thread | None" = None
        self._test_stop = threading.Event()
        self._test_interval = 0.5   # interval captured at test start (seconds)

        # debug toggles
        self.show_raw = tk.BooleanVar(value=False)
        self.show_heartbeat = tk.BooleanVar(value=False)
        self.autoscroll = tk.BooleanVar(value=True)

        # per-event-type filters: whether each MIDI message type shows up in
        # the console log and/or gets captured into a recording. Only
        # Program Change is on by default.
        self.log_filter = {code: tk.BooleanVar(value=(code in DEFAULT_VISIBLE_TYPES))
                            for code, _ in EVENT_TYPES}
        self.rec_filter = {code: tk.BooleanVar(value=(code in DEFAULT_VISIBLE_TYPES))
                            for code, _ in EVENT_TYPES}

        # link health
        self.last_rx_time = 0.0
        self.last_stat = {"in": 0, "out": 0, "err": 0}
        self.fw_id = "?"

        self._build_ui()
        self.refresh_ports()
        self.refresh_songs()
        if rtmidi is None:
            self.log("python-rtmidi not installed - playback can still go through the "
                      "ESP32; run `pip install python-rtmidi` to send it straight to a "
                      "system MIDI output (e.g. an iConnectMIDI2+).", "sys")
        self.refresh_playback_ports()
        self.root.after(self.POLL_MS, self._drain_queue)
        self.root.after(500, self._tick_status)
        self.root.after(50, self._tick_timeline)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------------- UI construction ----------------
    def _build_ui(self):
        pad = dict(padx=6, pady=4)

        # ---- Connection bar ----
        conn = ttk.LabelFrame(self.root, text="Connection")
        conn.pack(fill="x", **pad)

        # Transport selector (USB Serial vs WiFi TCP).
        ttk.Label(conn, text="Via:").grid(row=0, column=0, padx=4, pady=6, sticky="w")
        self.transport_cb = ttk.Combobox(conn, width=11, state="readonly",
                                          values=["USB Serial", "WiFi (TCP)"],
                                          textvariable=self.transport)
        self.transport_cb.grid(row=0, column=1, padx=4, pady=6)
        self.transport_cb.bind("<<ComboboxSelected>>", lambda e: self._update_transport_fields())

        # Serial-only widgets.
        self.port_lbl = ttk.Label(conn, text="Port:")
        self.port_lbl.grid(row=0, column=2, padx=(8, 2), sticky="w")
        self.port_cb = ttk.Combobox(conn, width=30, state="readonly")
        self.port_cb.grid(row=0, column=3, padx=2, pady=6)
        self.refresh_btn = ttk.Button(conn, text="Refresh", command=self.refresh_ports)
        self.refresh_btn.grid(row=0, column=4, padx=4)

        # WiFi-only widgets (hidden until WiFi is selected).
        self.host_lbl = ttk.Label(conn, text="Host:")
        self.host_entry = ttk.Entry(conn, width=15)
        self.host_entry.insert(0, WIFI_HOST)
        self.port_entry = ttk.Entry(conn, width=6)
        self.port_entry.insert(0, str(WIFI_PORT))

        self.connect_btn = ttk.Button(conn, text="Connect", command=self.toggle_connect)
        self.connect_btn.grid(row=0, column=7, padx=4)

        self.status_dot = tk.Canvas(conn, width=16, height=16, highlightthickness=0)
        self.status_dot.grid(row=0, column=8, padx=(14, 4))
        self._dot = self.status_dot.create_oval(2, 2, 14, 14, fill="#999", outline="")
        self.status_lbl = ttk.Label(conn, text="Disconnected")
        self.status_lbl.grid(row=0, column=9, padx=4, sticky="w")

        ttk.Button(conn, text="PING", command=self.send_ping).grid(row=0, column=10, padx=4)
        self.counters_lbl = ttk.Label(conn, text="in=0  out=0  err=0")
        self.counters_lbl.grid(row=0, column=11, padx=12, sticky="e")
        conn.columnconfigure(11, weight=1)

        # A second row holds a hint about the SoftAP credentials for WiFi mode.
        self.wifi_hint = ttk.Label(
            conn, foreground="#777",
            text=f"WiFi: join SSID \"{WIFI_SSID}\" (pass \"{WIFI_PASS}\"), then Connect to {WIFI_HOST}:{WIFI_PORT}")
        self._update_transport_fields()

        # ---- Timeline: PC-change layout + playhead for the current song ----
        tl_frame = ttk.LabelFrame(self.root, text="Timeline — Program Changes")
        tl_frame.pack(fill="x", **pad)
        self.timeline = TimelineView(tl_frame)
        self.timeline.canvas.pack(fill="x", expand=True, padx=4, pady=4)

        # ---- Main split: left = transport + active notes, right = console ----
        body = ttk.Frame(self.root)
        body.pack(fill="both", expand=True, **pad)

        left = ttk.Frame(body)
        left.pack(side="left", fill="y")

        # Song select — Record writes into whichever song is picked here.
        song = ttk.LabelFrame(left, text="Song")
        song.pack(fill="x", pady=(0, 6))
        ttk.Label(song, text="Current:").grid(row=0, column=0, padx=4, pady=4, sticky="w")
        self.song_cb = ttk.Combobox(song, state="readonly", width=16)
        self.song_cb.grid(row=0, column=1, padx=4, pady=4, sticky="ew")
        self.song_cb.bind("<<ComboboxSelected>>", lambda e: self.select_song(self.song_cb.get()))
        ttk.Button(song, text="New…", command=self.new_song_dialog).grid(row=0, column=2, padx=4, pady=4)
        song.columnconfigure(1, weight=1)

        # Playback destination — where Play/panic actually send events.
        # Independent of the connection above, which is only needed for
        # recording, the ESP32 output option, and the MIDI OUT wiring test.
        outp = ttk.LabelFrame(left, text="Playback Output")
        outp.pack(fill="x", pady=(0, 6))
        ttk.Label(outp, text="Send to:").grid(row=0, column=0, padx=4, pady=4, sticky="w")
        self.playback_cb = ttk.Combobox(outp, state="readonly", width=16,
                                        textvariable=self.playback_dest)
        self.playback_cb.grid(row=0, column=1, padx=4, pady=4, sticky="ew")
        self.playback_cb.bind("<<ComboboxSelected>>", lambda e: self._on_playback_dest_change())
        ttk.Button(outp, text="Refresh", command=self.refresh_playback_ports).grid(
            row=0, column=2, padx=4, pady=4)
        outp.columnconfigure(1, weight=1)

        # Transport / recording
        rec = ttk.LabelFrame(left, text="Recorder")
        rec.pack(fill="x", pady=(0, 6))
        self.record_btn = ttk.Button(rec, text="● Record", command=self.toggle_record)
        self.record_btn.grid(row=0, column=0, padx=4, pady=4, sticky="ew")
        self.play_btn = ttk.Button(rec, text="▶ Play", command=self.toggle_play)
        self.play_btn.grid(row=0, column=1, padx=4, pady=4, sticky="ew")
        ttk.Button(rec, text="⏹ All Notes Off", command=self.panic).grid(
            row=1, column=0, columnspan=2, padx=4, pady=2, sticky="ew")
        self.rec_status = ttk.Label(rec, text="idle — 0 events")
        self.rec_status.grid(row=2, column=0, columnspan=2, padx=4, pady=4, sticky="w")

        ttk.Label(rec, text="Speed:").grid(row=3, column=0, padx=4, sticky="e")
        self.speed = tk.DoubleVar(value=1.0)
        ttk.Spinbox(rec, from_=0.25, to=4.0, increment=0.25, width=6,
                    textvariable=self.speed).grid(row=3, column=1, padx=4, sticky="w")

        fbtns = ttk.Frame(rec)
        fbtns.grid(row=4, column=0, columnspan=2, pady=(6, 4), sticky="ew")
        ttk.Button(fbtns, text="Clear", command=self.clear_take).pack(side="left", padx=2)
        ttk.Button(fbtns, text="Save…", command=self.save_json).pack(side="left", padx=2)
        ttk.Button(fbtns, text="Load…", command=self.load_json).pack(side="left", padx=2)
        ttk.Button(fbtns, text="Export .mid", command=self.export_mid).pack(side="left", padx=2)

        # MIDI-out wiring test: pulses ch1 C4 at a set interval so hardware
        # MIDI OUT (and anything wired off it, e.g. the relay) can be confirmed.
        testf = ttk.Frame(rec)
        testf.grid(row=5, column=0, columnspan=2, pady=(2, 4), sticky="ew")
        self.test_btn = ttk.Button(testf, text="TEST MIDI OUT", command=self.toggle_test_midi)
        self.test_btn.pack(side="left", padx=2)
        ttk.Label(testf, text="every").pack(side="left", padx=(8, 2))
        self.test_interval_var = tk.DoubleVar(value=0.5)
        ttk.Spinbox(testf, from_=0.1, to=5.0, increment=0.1, width=5,
                    textvariable=self.test_interval_var).pack(side="left")
        ttk.Label(testf, text="s").pack(side="left", padx=(2, 0))

        # Multi-track record: replay the same song N times in a row so a
        # limited number of hardware inputs (e.g. capturing an Octatrack's 8
        # tracks 2 at a time) can be recorded across multiple passes.
        mt = ttk.LabelFrame(left, text="Multi-Track Record")
        mt.pack(fill="x", pady=(0, 6))
        self.multitrack_btn = ttk.Button(mt, text="Start Multi-Track", command=self.toggle_multitrack)
        self.multitrack_btn.grid(row=0, column=0, padx=4, pady=4, sticky="ew")
        ttk.Label(mt, text="Passes:").grid(row=0, column=1, padx=(8, 2), pady=4, sticky="e")
        self.multitrack_passes_var = tk.IntVar(value=4)
        ttk.Spinbox(mt, from_=1, to=16, width=4,
                    textvariable=self.multitrack_passes_var).grid(row=0, column=2, padx=(0, 4), pady=4)
        self.multitrack_status = ttk.Label(mt, text="idle")
        self.multitrack_status.grid(row=1, column=0, columnspan=3, padx=4, pady=(0, 4), sticky="w")
        mt.columnconfigure(0, weight=1)

        # Active notes display
        an = ttk.LabelFrame(left, text="Active notes")
        an.pack(fill="both", expand=True)
        self.active_list = tk.Listbox(an, height=10, width=28,
                                      font=("Consolas", 11))
        self.active_list.pack(fill="both", expand=True, padx=4, pady=4)

        # ---- Right: console + debug ----
        right = ttk.Frame(body)
        right.pack(side="left", fill="both", expand=True, padx=(8, 0))

        dbg = ttk.LabelFrame(right, text="Debug")
        dbg.pack(fill="x")
        ttk.Checkbutton(dbg, text="Show raw lines", variable=self.show_raw).pack(side="left", padx=6)
        ttk.Checkbutton(dbg, text="Show heartbeats", variable=self.show_heartbeat).pack(side="left", padx=6)
        ttk.Checkbutton(dbg, text="Auto-scroll", variable=self.autoscroll).pack(side="left", padx=6)
        ttk.Button(dbg, text="Clear log", command=self.clear_log).pack(side="right", padx=6)

        # ---- Per-type event filters (log visibility / recording inclusion) ----
        filt = ttk.LabelFrame(right, text="Event Filters")
        filt.pack(fill="x", pady=(6, 0))
        ttk.Label(filt, text="Log").grid(row=1, column=0, padx=4, sticky="e")
        ttk.Label(filt, text="Rec").grid(row=2, column=0, padx=4, sticky="e")
        for col, (code, _label) in enumerate(EVENT_TYPES, start=1):
            ttk.Label(filt, text=code).grid(row=0, column=col, padx=4, pady=(2, 0))
            ttk.Checkbutton(filt, variable=self.log_filter[code]).grid(row=1, column=col, padx=4)
            ttk.Checkbutton(filt, variable=self.rec_filter[code]).grid(row=2, column=col, padx=4)

        # Block specific PC program numbers outright (not logged, not recorded)
        # regardless of the Log/Rec checkboxes above — for noisy/junk PC values
        # a controller sends that you never want to see at all.
        ttk.Label(filt, text="Block PC #:").grid(row=3, column=0, padx=4, pady=(6, 4), sticky="e")
        self.pc_block_var = tk.StringVar(value="")
        ttk.Entry(filt, textvariable=self.pc_block_var, width=20).grid(
            row=3, column=1, columnspan=len(EVENT_TYPES) - 1, padx=4, pady=(6, 4), sticky="w")
        ttk.Label(filt, text="e.g. 127", foreground="#777").grid(
            row=3, column=len(EVENT_TYPES), padx=4, pady=(6, 4), sticky="w")

        consf = ttk.LabelFrame(right, text="Console")
        consf.pack(fill="both", expand=True, pady=(6, 0))
        self.console = tk.Text(consf, wrap="none", height=20, state="disabled",
                               font=("Consolas", 10), background="#101418",
                               foreground="#d6deeb")
        yscroll = ttk.Scrollbar(consf, command=self.console.yview)
        self.console.configure(yscrollcommand=yscroll.set)
        yscroll.pack(side="right", fill="y")
        self.console.pack(side="left", fill="both", expand=True)
        for tag, color in (("rx", "#9ad0ff"), ("tx", "#c3e88d"),
                           ("evt", "#ffcb6b"), ("err", "#ff6b6b"),
                           ("sys", "#80868b")):
            self.console.tag_configure(tag, foreground=color)

        # Manual command entry
        cmdf = ttk.Frame(right)
        cmdf.pack(fill="x", pady=(6, 0))
        ttk.Label(cmdf, text="Send:").pack(side="left")
        self.cmd_entry = ttk.Entry(cmdf)
        self.cmd_entry.pack(side="left", fill="x", expand=True, padx=4)
        self.cmd_entry.bind("<Return>", lambda e: self.send_manual())
        ttk.Button(cmdf, text="Send", command=self.send_manual).pack(side="left")

    # ---------------- Connection ----------------
    def _is_wifi(self) -> bool:
        return self.transport.get().startswith("WiFi")

    def _update_transport_fields(self):
        """Show the Port widgets for Serial, or the Host/Port widgets for WiFi."""
        wifi = self._is_wifi()
        # Serial widgets occupy columns 2..4; WiFi widgets reuse them.
        for w in (self.port_lbl, self.port_cb, self.refresh_btn):
            w.grid_remove() if wifi else w.grid()
        if wifi:
            self.host_lbl.grid(row=0, column=2, padx=(8, 2), sticky="w")
            self.host_entry.grid(row=0, column=3, padx=2, sticky="w")
            self.port_entry.grid(row=0, column=4, padx=2, sticky="w")
            self.wifi_hint.grid(row=1, column=0, columnspan=12, padx=6, pady=(0, 4), sticky="w")
        else:
            self.host_lbl.grid_remove()
            self.host_entry.grid_remove()
            self.port_entry.grid_remove()
            self.wifi_hint.grid_remove()

    def refresh_ports(self):
        ports = list_ports.comports()
        items = [f"{p.device}  —  {p.description}" for p in ports]
        self._port_map = {f"{p.device}  —  {p.description}": p.device for p in ports}
        self.port_cb["values"] = items
        if items and not self.port_cb.get():
            self.port_cb.current(0)
        self.log(f"Found {len(items)} serial port(s).", "sys")

    def toggle_connect(self):
        if self.link.is_open:
            self.link.close()
            self.set_status(False)
            self.connect_btn.config(text="Connect")
            self._set_transport_enabled(True)
            self.log("Disconnected.", "sys")
            return

        if self._is_wifi():
            self.link = self.tcp_link
            host = self.host_entry.get().strip() or WIFI_HOST
            try:
                port = int(self.port_entry.get().strip() or WIFI_PORT)
            except ValueError:
                messagebox.showwarning("Bad port", "TCP port must be a number.")
                return
            try:
                self.link.open(host, port)
            except Exception as exc:
                messagebox.showerror("Connect failed",
                                     f"Couldn't reach {host}:{port}\n\n{exc}\n\n"
                                     f"Joined the \"{WIFI_SSID}\" WiFi network?")
                self.log(f"WiFi connect failed: {exc}", "err")
                return
            target = f"{host}:{port}"
        else:
            self.link = self.serial_link
            sel = self.port_cb.get()
            port = getattr(self, "_port_map", {}).get(sel, sel.split(" ")[0] if sel else "")
            if not port:
                messagebox.showwarning("No port", "Select a serial port first.")
                return
            try:
                self.link.open(port, BAUD)
            except Exception as exc:
                messagebox.showerror("Connect failed", str(exc))
                self.log(f"Connect failed: {exc}", "err")
                return
            target = f"{port} @ {BAUD}"

        self.set_status(True, "Connecting…")
        self.connect_btn.config(text="Disconnect")
        self._set_transport_enabled(False)
        self.last_rx_time = time.monotonic()
        self.log(f"Opened {target}.", "sys")

    def _set_transport_enabled(self, enabled: bool):
        # Lock the transport selector while connected so link/active don't diverge.
        self.transport_cb.config(state="readonly" if enabled else "disabled")

    def set_status(self, connected: bool, text: str = None):
        if connected:
            color = "#36c275"  # green
            label = text or f"Connected ({self.fw_id})"
        else:
            color = "#999"
            label = text or "Disconnected"
        self.status_dot.itemconfig(self._dot, fill=color)
        self.status_lbl.config(text=label)

    def send_ping(self):
        if self._require_link():
            self.link.write_line("PING")
            self.log("PING", "tx")

    # ---------------- Incoming line handling ----------------
    def _drain_queue(self):
        try:
            while True:
                kind, payload = self.line_queue.get_nowait()
                if kind == "line":
                    self._handle_line(payload)
                elif kind == "closed":
                    self.set_status(False)
                    self.connect_btn.config(text="Connect")
                    self._set_transport_enabled(True)
                    self.log("Link closed.", "sys")
                elif kind == "error":
                    self.log(payload, "err")
        except queue.Empty:
            pass
        self.root.after(self.POLL_MS, self._drain_queue)

    def _handle_line(self, line: str):
        self.last_rx_time = time.monotonic()
        token = line.split(" ", 1)[0]

        if token == "EVT":
            self._handle_event(line)
        elif token == "STAT":
            self._handle_stat(line)
            if self.show_heartbeat.get():
                self.log(line, "rx")
        elif token == "READY":
            parts = line.split()
            self.fw_id = " ".join(parts[1:]) if len(parts) > 1 else "ready"
            self.set_status(True)
            self.log(line, "sys")
        elif token == "PONG":
            self.set_status(True)
            self.log(f"{line}   (link OK)", "rx")
        elif token in ("ECHO",):
            self.log(line, "rx")
        elif line.startswith("#"):
            if self.show_raw.get():
                self.log(line, "sys")
        else:
            if self.show_raw.get():
                self.log(line, "rx")

    def _handle_event(self, line: str):
        # EVT <ms> <TYPE> <ch> <d1> <d2>
        parts = line.split()
        if len(parts) < 6:
            return
        try:
            ms = int(parts[1]); etype = parts[2]
            ch = int(parts[3]); d1 = int(parts[4]); d2 = int(parts[5])
        except ValueError:
            return

        if etype == "PC" and d1 in self._blocked_pc_values():
            return   # fully ignored: not logged, not recorded

        # Record using the BOARD's timestamp (the ms field), not host arrival
        # time, so WiFi/USB latency and jitter don't smear the captured timing.
        # Gated per-type: only event types with "Rec" checked are captured, so
        # the anchor is only set on the first *recorded* event, not the first
        # event of any kind.
        rec_var = self.rec_filter.get(etype)
        if self.recording and rec_var is not None and rec_var.get():
            if self.rec_start_ms is None:
                self.rec_start_ms = ms          # anchor on the first recorded event
            self.events.append({
                "t": max(0.0, (ms - self.rec_start_ms) / 1000.0),
                "ms": ms, "type": etype, "ch": ch, "d1": d1, "d2": d2,
            })
            self.rec_status.config(text=f"● REC — {len(self.events)} events")

        # Live active-note tracking (independent of the log/record filters —
        # this is a real-time performance monitor, not a log).
        if etype == "NON" and d2 > 0:
            self.active_notes[(ch, d1)] = d2
        elif etype == "NOF" or (etype == "NON" and d2 == 0):
            self.active_notes.pop((ch, d1), None)
        self._refresh_active()

        # Console line (human readable), gated per-type by the "Log" filter.
        log_var = self.log_filter.get(etype)
        if log_var is not None and log_var.get():
            self.log(self._format_event(etype, ch, d1, d2), "evt")
        elif self.show_raw.get():
            self.log(line, "evt")

    def _format_event(self, etype: str, ch: int, d1: int, d2: int) -> str:
        """Human-readable rendering of one MIDI event for the console."""
        if etype in ("NON", "NOF"):
            return f"{etype}  ch{ch:<2} {note_name(d1):>4} ({d1:>3}) vel={d2:<3}"
        if etype == "CC":
            return f"CC   ch{ch:<2} ctrl={d1:<3} val={d2:<3}"
        if etype == "PC":
            return f"PC   ch{ch:<2} prog={d1:<3}"
        if etype == "PB":
            return f"PB   ch{ch:<2} bend={d1:<5} (0..16383)"
        if etype == "AT":
            return f"AT   ch{ch:<2} {note_name(d1):>4} ({d1:>3}) press={d2}"
        if etype == "CAT":
            return f"CAT  ch{ch:<2} press={d1}"
        return f"{etype}  ch{ch} d1={d1} d2={d2}"

    def _blocked_pc_values(self) -> set:
        """Program-Change numbers to fully ignore, from the "Block PC #:" field
        (comma/space-separated, e.g. "127" or "0, 127")."""
        out = set()
        for tok in re.split(r"[,\s]+", self.pc_block_var.get().strip()):
            if not tok:
                continue
            try:
                out.add(int(tok))
            except ValueError:
                pass
        return out

    def _handle_stat(self, line: str):
        for part in line.split():
            if "=" in part:
                k, v = part.split("=", 1)
                if k in self.last_stat:
                    try:
                        self.last_stat[k] = int(v)
                    except ValueError:
                        pass
        self.counters_lbl.config(
            text=f"in={self.last_stat['in']}  out={self.last_stat['out']}  err={self.last_stat['err']}")

    def _refresh_active(self):
        self.active_list.delete(0, tk.END)
        for (ch, note), vel in sorted(self.active_notes.items()):
            self.active_list.insert(tk.END, f"ch{ch:<2} {note_name(note):>4} ({note:>3}) vel={vel}")

    # ---------------- Playback output ----------------
    def refresh_playback_ports(self):
        ports = [ESP32_OUTPUT_LABEL]
        if rtmidi is not None:
            try:
                ports += rtmidi.MidiOut().get_ports()
            except Exception as exc:
                self.log(f"MIDI output scan failed: {exc}", "err")
        self.playback_cb["values"] = ports
        if self.playback_dest.get() not in ports:
            self.playback_dest.set(ESP32_OUTPUT_LABEL)
            self._on_playback_dest_change()
        return ports

    def _on_playback_dest_change(self):
        dest = self.playback_dest.get()
        self._close_midiout()
        if dest == ESP32_OUTPUT_LABEL or rtmidi is None:
            self.log(f"Playback output -> {ESP32_OUTPUT_LABEL}.", "sys")
            return
        try:
            names = rtmidi.MidiOut().get_ports()
            out = rtmidi.MidiOut()
            out.open_port(names.index(dest))
            self.midiout = out
            self.log(f"Playback output -> \"{dest}\".", "sys")
        except Exception as exc:
            messagebox.showerror("MIDI output failed", f"Couldn't open \"{dest}\":\n{exc}")
            self.log(f"Failed to open MIDI output \"{dest}\": {exc}", "err")
            self.playback_dest.set(ESP32_OUTPUT_LABEL)

    def _close_midiout(self):
        if self.midiout is not None:
            try:
                self.midiout.close_port()
            except Exception:
                pass
            self.midiout = None

    def _require_playback_ready(self) -> bool:
        if self.playback_dest.get() == ESP32_OUTPUT_LABEL:
            return self._require_link()
        if self.midiout is None:
            messagebox.showwarning("No MIDI output", "Select a MIDI output port first.")
            return False
        return True

    def _all_notes_off_playback(self, dest: "str | None" = None):
        # dest lets the background playback thread pass its plain-str snapshot
        # instead of touching the playback_dest Tk Var from off the main thread.
        if dest is None:
            dest = self.playback_dest.get()
        if dest == ESP32_OUTPUT_LABEL:
            if self.link.is_open:
                self.link.write_line("ALLOFF")
        elif self.midiout is not None:
            for ch in range(16):
                self.midiout.send_message([0xB0 | ch, 123, 0])

    # ---------------- Songs ----------------
    def _song_path(self, name: str) -> str:
        return os.path.join(SONGS_DIR, f"{name}.json")

    def _sanitize_song_name(self, name: str) -> str:
        name = name.strip()
        return re.sub(r'[\\/:*?"<>|]', "", name)

    def _write_song(self, name: str, events: list):
        os.makedirs(SONGS_DIR, exist_ok=True)
        with open(self._song_path(name), "w", encoding="utf-8") as fh:
            json.dump({"version": 1, "events": events}, fh, indent=2)

    def refresh_songs(self):
        os.makedirs(SONGS_DIR, exist_ok=True)
        names = sorted(os.path.splitext(f)[0] for f in os.listdir(SONGS_DIR)
                        if f.lower().endswith(".json"))
        self.song_cb["values"] = names
        return names

    def new_song_dialog(self):
        if self.recording:
            messagebox.showinfo("Recording", "Stop recording first.")
            return
        name = simpledialog.askstring("New song", "Song name:", parent=self.root)
        if not name:
            return
        name = self._sanitize_song_name(name)
        if not name:
            messagebox.showwarning("Invalid name", "Song name can't be empty.")
            return
        if os.path.exists(self._song_path(name)) and not messagebox.askyesno(
                "Overwrite song?",
                f"A song named \"{name}\" already exists. Overwrite it with a new, empty song?"):
            return
        self._write_song(name, [])
        self.refresh_songs()
        self.select_song(name)
        self.log(f"Created song \"{name}\".", "sys")

    def select_song(self, name: str):
        if not name:
            return
        if self.recording:
            messagebox.showinfo("Recording", "Stop recording before switching songs.")
            self.song_cb.set(self.current_song or "")
            return
        try:
            with open(self._song_path(name), "r", encoding="utf-8") as fh:
                events = json.load(fh)["events"]
        except FileNotFoundError:
            events = []
        except Exception as exc:
            messagebox.showerror("Load failed", str(exc))
            return
        self.current_song = name
        self.events = events
        self.song_cb.set(name)
        self.rec_status.config(text=f"idle — \"{name}\" — {len(self.events)} events")
        self.timeline.set_events(self.events)
        self.timeline.set_playhead(0.0)
        self.log(f"Loaded song \"{name}\" ({len(self.events)} events).", "sys")

    # ---------------- Recording ----------------
    def toggle_record(self):
        if self.recording:
            self.recording = False
            self.record_btn.config(text="● Record")
            if self.current_song:
                self._write_song(self.current_song, self.events)
                self.rec_status.config(text=f"stopped — \"{self.current_song}\" — {len(self.events)} events")
                self.log(f"Recording stopped: saved {len(self.events)} events to \"{self.current_song}\".", "sys")
            else:
                self.rec_status.config(text=f"stopped — {len(self.events)} events")
                self.log(f"Recording stopped: {len(self.events)} events.", "sys")
            self.timeline.set_events(self.events)
        else:
            if not self.current_song:
                messagebox.showwarning("No song selected", "Select or create a song first.")
                return
            if self.events and not messagebox.askyesno(
                    "Overwrite song?",
                    f"Recording will overwrite the {len(self.events)}-event take saved in "
                    f"\"{self.current_song}\". Continue?"):
                return
            self.events = []
            self.recording = True
            self.rec_start_ms = None   # anchored on the first event's board ms
            self.record_btn.config(text="■ Stop Rec")
            self.rec_status.config(text=f"● REC — \"{self.current_song}\" — 0 events")
            self.timeline.set_events([])
            self.timeline.set_playhead(0.0)
            self.log(f"Recording started into \"{self.current_song}\".", "sys")

    def clear_take(self):
        if self.recording:
            messagebox.showinfo("Recording", "Stop recording first.")
            return
        self.events = []
        self.timeline.set_events(self.events)
        self.timeline.set_playhead(0.0)
        if self.current_song:
            self.rec_status.config(text=f"idle — \"{self.current_song}\" — 0 events")
        else:
            self.rec_status.config(text="idle — 0 events")
        self.log("Take cleared.", "sys")

    # ---------------- Playback ----------------
    def toggle_play(self):
        if self.playing:
            self._play_stop.set()
            return
        if not self.events:
            messagebox.showinfo("Nothing to play", "Record or load a take first.")
            return
        if not self._require_playback_ready():
            return
        self._start_playback()

    def _start_playback(self):
        self.playing = True
        self._play_speed = max(0.05, self.speed.get())
        self._play_dest = self.playback_dest.get()
        self._play_t0 = time.monotonic()
        self.timeline.set_playhead(0.0)
        self.play_btn.config(text="■ Stop")
        self._play_stop.clear()
        self._play_thread = threading.Thread(target=self._play_loop, daemon=True)
        self._play_thread.start()
        self.log(f"Playback started ({len(self.events)} events, x{self.speed.get()}).", "sys")

    def _play_loop(self):
        speed = self._play_speed
        events = sorted(self.events, key=lambda e: e["t"])
        t0 = self._play_t0
        for ev in events:
            if self._play_stop.is_set():
                break
            target = ev["t"] / speed
            while not self._play_stop.is_set():
                ahead = target - (time.monotonic() - t0)
                if ahead <= 0:
                    break
                time.sleep(min(ahead, 0.01))
            if self._play_stop.is_set():
                break
            self._send_event(ev)
        # leave nothing hanging
        stopped_early = self._play_stop.is_set()
        self._all_notes_off_playback(self._play_dest)
        self.line_queue.put(("line", "# playback finished"))
        self.root.after(0, self._play_done, stopped_early)

    def _send_event(self, ev: dict):
        # Runs on the background playback thread: use the plain-str snapshot
        # (self._play_dest), never self.playback_dest.get() (a Tk Var).
        if self._play_dest != ESP32_OUTPUT_LABEL:
            if self.midiout is not None:
                msg = midi_bytes(ev["type"], ev["ch"], ev["d1"], ev["d2"])
                if msg is not None:
                    self.midiout.send_message(list(msg))
            return
        t, ch, d1, d2 = ev["type"], ev["ch"], ev["d1"], ev["d2"]
        if t == "NON":
            self.link.write_line(f"PNON {ch} {d1} {d2}")
        elif t == "NOF":
            self.link.write_line(f"PNOF {ch} {d1} {d2}")
        elif t == "CC":
            self.link.write_line(f"PCC {ch} {d1} {d2}")
        elif t == "PB":
            self.link.write_line(f"PPB {ch} {d1}")
        elif t == "PC":
            self.link.write_line(f"PPC {ch} {d1}")

    def _play_done(self, stopped_early: bool = False):
        self.playing = False
        self._play_t0 = None
        self.play_btn.config(text="▶ Play")
        self.timeline.set_playhead(0.0)
        self.log("Playback stopped.", "sys")
        if self.multitrack_active:
            if stopped_early:
                self._cancel_multitrack("Multi-track cancelled: playback stopped early.")
            else:
                self._multitrack_advance()

    def panic(self):
        if self._require_playback_ready():
            self._all_notes_off_playback()
            self.active_notes.clear()
            self._refresh_active()
            self.log("ALLOFF (panic) sent.", "tx")

    # ---------------- Multi-track record ----------------
    def toggle_multitrack(self):
        if self.multitrack_active:
            self._play_stop.set()   # _play_done() will see this and cancel cleanly
            self._cancel_multitrack("Multi-track cancelled.")
            return
        if self.playing:
            messagebox.showinfo("Playback in progress", "Stop playback first.")
            return
        if not self.events:
            messagebox.showinfo("Nothing to play", "Record or load a take first.")
            return
        if not self._require_playback_ready():
            return
        self.multitrack_total = max(1, int(self.multitrack_passes_var.get()))
        self.multitrack_pass = 1
        self.multitrack_active = True
        self.multitrack_btn.config(text="■ Stop Multi-Track")
        self.multitrack_status.config(text=f"Multi-track: pass {self.multitrack_pass}/{self.multitrack_total}")
        self.log(f"Multi-track: starting pass {self.multitrack_pass}/{self.multitrack_total}.", "sys")
        self._start_playback()

    def _multitrack_advance(self):
        completed = self.multitrack_pass
        if completed >= self.multitrack_total:
            self.multitrack_active = False
            self.multitrack_btn.config(text="Start Multi-Track")
            self.multitrack_status.config(text=f"Multi-track complete ({self.multitrack_total}/{self.multitrack_total}).")
            self.log("Multi-track recording complete.", "sys")
            messagebox.showinfo("Multi-track complete",
                                 f"All {self.multitrack_total} passes finished.")
            return
        self.multitrack_pass += 1
        proceed = messagebox.askokcancel(
            "Ready for next set of tracks",
            f"Pass {completed} of {self.multitrack_total} finished.\n\n"
            f"Reconfigure the Octatrack outputs / your recorder inputs for the "
            f"next set of tracks, then click OK to start pass "
            f"{self.multitrack_pass} of {self.multitrack_total}.")
        if not proceed:
            self._cancel_multitrack(f"Multi-track cancelled after pass {completed}/{self.multitrack_total}.")
            return
        self.multitrack_status.config(text=f"Multi-track: pass {self.multitrack_pass}/{self.multitrack_total}")
        self.log(f"Multi-track: starting pass {self.multitrack_pass}/{self.multitrack_total}.", "sys")
        self._start_playback()

    def _cancel_multitrack(self, message: str):
        self.multitrack_active = False
        self.multitrack_btn.config(text="Start Multi-Track")
        self.multitrack_status.config(text=message)
        self.log(message, "sys")

    # ---------------- MIDI-out wiring test ----------------
    def toggle_test_midi(self):
        if self.testing_midi:
            self._test_stop.set()
            return
        if not self._require_link():
            return
        self.testing_midi = True
        self._test_interval = max(0.05, self.test_interval_var.get())
        self._test_stop.clear()
        self.test_btn.config(text="■ Stop Test")
        self._test_thread = threading.Thread(target=self._test_loop, daemon=True)
        self._test_thread.start()
        self.log(f"TEST MIDI OUT started (ch1 note C4 every {self._test_interval:g}s).", "sys")

    def _test_loop(self):
        ch, note, vel = 1, 60, 100
        on_time = min(0.1, self._test_interval / 2)
        while not self._test_stop.is_set():
            self.link.write_line(f"PNON {ch} {note} {vel}")
            if self._test_stop.wait(on_time):
                break
            self.link.write_line(f"PNOF {ch} {note} 0")
            if self._test_stop.wait(self._test_interval - on_time):
                break
        self.link.write_line(f"PNOF {ch} {note} 0")   # don't leave a stuck note
        self.root.after(0, self._test_done)

    def _test_done(self):
        self.testing_midi = False
        self.test_btn.config(text="TEST MIDI OUT")
        self.log("TEST MIDI OUT stopped.", "sys")

    # ---------------- Save / load / export ----------------
    def save_json(self):
        if not self.events:
            messagebox.showinfo("Empty", "Nothing to save.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".json", filetypes=[("JSON recording", "*.json")],
            initialfile=time.strftime("take_%Y%m%d_%H%M%S.json"))
        if not path:
            return
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"version": 1, "events": self.events}, fh, indent=2)
        self.log(f"Saved {len(self.events)} events -> {os.path.basename(path)}", "sys")

    def load_json(self):
        path = filedialog.askopenfilename(filetypes=[("JSON recording", "*.json")])
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            self.events = data["events"]
        except Exception as exc:
            messagebox.showerror("Load failed", str(exc))
            return
        self.rec_status.config(text=f"loaded — {len(self.events)} events")
        self.timeline.set_events(self.events)
        self.timeline.set_playhead(0.0)
        self.log(f"Loaded {len(self.events)} events <- {os.path.basename(path)}", "sys")

    def export_mid(self):
        if not self.events:
            messagebox.showinfo("Empty", "Nothing to export.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".mid", filetypes=[("Standard MIDI File", "*.mid")],
            initialfile=time.strftime("take_%Y%m%d_%H%M%S.mid"))
        if not path:
            return
        try:
            export_smf(path, self.events)
        except Exception as exc:
            messagebox.showerror("Export failed", str(exc))
            return
        self.log(f"Exported -> {os.path.basename(path)}", "sys")

    # ---------------- Console / misc ----------------
    def send_manual(self):
        text = self.cmd_entry.get().strip()
        if not text:
            return
        if self._require_link():
            self.link.write_line(text)
            self.log(text, "tx")
        self.cmd_entry.delete(0, tk.END)

    def _require_link(self) -> bool:
        if not self.link.is_open:
            messagebox.showwarning("Not connected", "Connect to the board first.")
            return False
        return True

    def log(self, text: str, tag: str = "rx"):
        ts = time.strftime("%H:%M:%S")
        self.console.configure(state="normal")
        self.console.insert(tk.END, f"{ts}  ", "sys")
        self.console.insert(tk.END, text + "\n", tag)
        # cap the buffer so long sessions don't grow unbounded
        if int(self.console.index("end-1c").split(".")[0]) > 2000:
            self.console.delete("1.0", "500.0")
        if self.autoscroll.get():
            self.console.see(tk.END)
        self.console.configure(state="disabled")

    def clear_log(self):
        self.console.configure(state="normal")
        self.console.delete("1.0", tk.END)
        self.console.configure(state="disabled")

    def _tick_status(self):
        # Grey the dot if we've heard nothing for a while despite an open port.
        if self.link.is_open:
            silent = time.monotonic() - self.last_rx_time
            if silent > 4.0:
                self.status_dot.itemconfig(self._dot, fill="#e0a93b")  # amber
                self.status_lbl.config(text=f"No data {silent:0.0f}s — check wiring/baud")
            elif self.status_lbl.cget("text").startswith("No data"):
                self.set_status(True)
        self.root.after(500, self._tick_status)

    def _tick_timeline(self):
        # Drives the playhead: continuous during playback (elapsed wall time
        # x speed), or snapped to the latest event while recording. Polling
        # here (instead of redrawing per-event) caps redraw rate regardless
        # of how fast MIDI events stream in.
        if self.playing and self._play_t0 is not None:
            elapsed = (time.monotonic() - self._play_t0) * self._play_speed
            self.timeline.set_playhead(elapsed)
        elif self.recording:
            self.timeline.set_events(self.events)
            if self.events:
                self.timeline.set_playhead(self.events[-1]["t"])
        self.root.after(50, self._tick_timeline)

    def on_close(self):
        self._play_stop.set()
        self._test_stop.set()
        try:
            if self.link.is_open:
                self.link.write_line("ALLOFF")
        except Exception:
            pass
        try:
            self._all_notes_off_playback()
        except Exception:
            pass
        self._close_midiout()
        self.link.close()
        self.root.destroy()


def main():
    root = tk.Tk()
    # Use a slightly nicer theme where available.
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    RecorderApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
