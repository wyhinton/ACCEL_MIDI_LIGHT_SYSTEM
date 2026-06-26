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
  * PLAYS BACK a recording by streaming timed commands to the board, which
    emits them out the FeatherWing MIDI OUT jack,
  * SAVES / LOADS recordings as JSON, and exports a Standard MIDI File (.mid),
  * has a raw console + debug toggles (PING test, show heartbeats, show raw,
    send manual commands) so you can see exactly what the link is doing.

Dependencies:  pyserial   (pip install pyserial)
tkinter ships with CPython on Windows/macOS and most Linux python3 packages.

Protocol (must match MIDI_NOTE_LIGHT_REAL/src/main.cpp):
  Board -> PC : READY ... | #info | EVT <ms> <TYPE> <ch> <d1> <d2> | STAT ... | PONG ... | ECHO ...
  PC -> Board : PING | ECHO x | STAT | PNON/PNOF/PCC/PPB/PPC | ALLOFF [ch] | RAW <hex...>
"""

import json
import os
import queue
import struct
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - import guard for a friendlier message
    raise SystemExit(
        "pyserial is required.  Install it with:  pip install pyserial"
    )

BAUD = 115200
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


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
        ch = max(0, min(15, int(ev["ch"]) - 1))  # protocol is 1..16 -> 0..15
        t = ev["type"]
        d1, d2 = int(ev["d1"]), int(ev["d2"])
        if t == "NON":
            msg = bytes([0x90 | ch, d1 & 0x7F, d2 & 0x7F])
        elif t == "NOF":
            msg = bytes([0x80 | ch, d1 & 0x7F, d2 & 0x7F])
        elif t == "CC":
            msg = bytes([0xB0 | ch, d1 & 0x7F, d2 & 0x7F])
        elif t == "PC":
            msg = bytes([0xC0 | ch, d1 & 0x7F])
        elif t == "PB":
            val = max(0, min(16383, d1))
            msg = bytes([0xE0 | ch, val & 0x7F, (val >> 7) & 0x7F])
        elif t == "CAT":
            msg = bytes([0xD0 | ch, d1 & 0x7F])
        elif t == "AT":
            msg = bytes([0xA0 | ch, d1 & 0x7F, d2 & 0x7F])
        else:
            continue
        track += _vlq(delta) + msg

    track += _vlq(0) + b"\xFF\x2F\x00"  # end of track

    with open(path, "wb") as fh:
        fh.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppq))
        fh.write(b"MTrk" + struct.pack(">I", len(track)) + bytes(track))


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
        self.link = SerialLink(self.line_queue)

        # recording state
        self.recording = False
        self.rec_start = 0.0
        self.events: list = []          # captured events (current take)
        self.active_notes: dict = {}    # (ch, note) -> velocity, for live display

        # playback state
        self.playing = False
        self._play_thread: "threading.Thread | None" = None
        self._play_stop = threading.Event()

        # debug toggles
        self.show_raw = tk.BooleanVar(value=False)
        self.show_heartbeat = tk.BooleanVar(value=False)
        self.autoscroll = tk.BooleanVar(value=True)

        # link health
        self.last_rx_time = 0.0
        self.last_stat = {"in": 0, "out": 0, "err": 0}
        self.fw_id = "?"

        self._build_ui()
        self.refresh_ports()
        self.root.after(self.POLL_MS, self._drain_queue)
        self.root.after(500, self._tick_status)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------------- UI construction ----------------
    def _build_ui(self):
        pad = dict(padx=6, pady=4)

        # ---- Connection bar ----
        conn = ttk.LabelFrame(self.root, text="Connection")
        conn.pack(fill="x", **pad)

        ttk.Label(conn, text="Port:").grid(row=0, column=0, padx=4, pady=6, sticky="w")
        self.port_cb = ttk.Combobox(conn, width=34, state="readonly")
        self.port_cb.grid(row=0, column=1, padx=4, pady=6)
        ttk.Button(conn, text="Refresh", command=self.refresh_ports).grid(row=0, column=2, padx=4)
        self.connect_btn = ttk.Button(conn, text="Connect", command=self.toggle_connect)
        self.connect_btn.grid(row=0, column=3, padx=4)

        self.status_dot = tk.Canvas(conn, width=16, height=16, highlightthickness=0)
        self.status_dot.grid(row=0, column=4, padx=(14, 4))
        self._dot = self.status_dot.create_oval(2, 2, 14, 14, fill="#999", outline="")
        self.status_lbl = ttk.Label(conn, text="Disconnected")
        self.status_lbl.grid(row=0, column=5, padx=4, sticky="w")

        ttk.Button(conn, text="PING", command=self.send_ping).grid(row=0, column=6, padx=4)
        self.counters_lbl = ttk.Label(conn, text="in=0  out=0  err=0")
        self.counters_lbl.grid(row=0, column=7, padx=12, sticky="e")
        conn.columnconfigure(7, weight=1)

        # ---- Main split: left = transport + active notes, right = console ----
        body = ttk.Frame(self.root)
        body.pack(fill="both", expand=True, **pad)

        left = ttk.Frame(body)
        left.pack(side="left", fill="y")

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
            self.log("Disconnected.", "sys")
            return
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
        self.set_status(True, "Connecting…")
        self.connect_btn.config(text="Disconnect")
        self.last_rx_time = time.monotonic()
        self.log(f"Opened {port} @ {BAUD}.", "sys")

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
                    self.log("Serial port closed.", "sys")
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

        # Record (host clock relative to record start).
        if self.recording:
            self.events.append({
                "t": time.monotonic() - self.rec_start,
                "ms": ms, "type": etype, "ch": ch, "d1": d1, "d2": d2,
            })
            self.rec_status.config(text=f"● REC — {len(self.events)} events")

        # Live active-note tracking.
        if etype == "NON" and d2 > 0:
            self.active_notes[(ch, d1)] = d2
        elif etype == "NOF" or (etype == "NON" and d2 == 0):
            self.active_notes.pop((ch, d1), None)
        self._refresh_active()

        # Console line (human readable).
        if etype in ("NON", "NOF"):
            human = f"ch{ch:<2} {note_name(d1):>4} ({d1:>3}) vel={d2:<3}"
            self.log(f"{etype}  {human}", "evt")
        elif self.show_raw.get():
            self.log(line, "evt")

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

    # ---------------- Recording ----------------
    def toggle_record(self):
        if self.recording:
            self.recording = False
            self.record_btn.config(text="● Record")
            self.rec_status.config(text=f"stopped — {len(self.events)} events")
            self.log(f"Recording stopped: {len(self.events)} events.", "sys")
        else:
            if self.events and not messagebox.askyesno(
                    "Overwrite take?",
                    f"Discard the current {len(self.events)}-event take and record fresh?"):
                return
            self.events = []
            self.recording = True
            self.rec_start = time.monotonic()
            self.record_btn.config(text="■ Stop Rec")
            self.rec_status.config(text="● REC — 0 events")
            self.log("Recording started.", "sys")

    def clear_take(self):
        if self.recording:
            messagebox.showinfo("Recording", "Stop recording first.")
            return
        self.events = []
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
        if not self._require_link():
            return
        self.playing = True
        self.play_btn.config(text="■ Stop")
        self._play_stop.clear()
        self._play_thread = threading.Thread(target=self._play_loop, daemon=True)
        self._play_thread.start()
        self.log(f"Playback started ({len(self.events)} events, x{self.speed.get()}).", "sys")

    def _play_loop(self):
        speed = max(0.05, self.speed.get())
        events = sorted(self.events, key=lambda e: e["t"])
        t0 = time.monotonic()
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
        self.link.write_line("ALLOFF")
        self.line_queue.put(("line", "# playback finished"))
        self.root.after(0, self._play_done)

    def _send_event(self, ev: dict):
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

    def _play_done(self):
        self.playing = False
        self.play_btn.config(text="▶ Play")
        self.log("Playback stopped.", "sys")

    def panic(self):
        if self._require_link():
            self.link.write_line("ALLOFF")
            self.active_notes.clear()
            self._refresh_active()
            self.log("ALLOFF (panic) sent.", "tx")

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

    def on_close(self):
        self._play_stop.set()
        try:
            if self.link.is_open:
                self.link.write_line("ALLOFF")
        except Exception:
            pass
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
