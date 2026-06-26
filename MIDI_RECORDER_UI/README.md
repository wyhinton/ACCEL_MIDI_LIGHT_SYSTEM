# MIDI Recorder UI

A tkinter desktop app that turns the **MIDI_NOTE_LIGHT_REAL** board into a MIDI
note recorder. This lives on the `midi_rec_for_recording` branch, where that
board's firmware does **nothing with lights/relay/ESP-NOW** — it is purely a
MIDI ⇄ USB bridge.

```
  MIDI controller / keyboard
        │  (DIN MIDI, 31250 baud)
        ▼
  MIDI FeatherWing ── Serial1 ──▶ ESP32-S3 ──(USB 115200, ASCII lines)──▶ this app
                                     ▲                                         │
                                     └──────────── playback commands ◀─────────┘
                                                       │
                                                       ▼
                                            MIDI FeatherWing MIDI OUT jack
```

## Setup

```sh
cd MIDI_RECORDER_UI
python -m venv .venv            # optional but recommended
.venv\Scripts\activate         # Windows (use source .venv/bin/activate on *nix)
pip install -r requirements.txt
python midi_recorder.py
```

`tkinter` ships with the standard CPython installer on Windows/macOS. On Debian/
Ubuntu install it with `sudo apt install python3-tk`. The only pip dependency is
`pyserial`.

## Flash the board first

Build/upload the bridge firmware (PlatformIO):

```sh
pio run --target upload --project-dir MIDI_NOTE_LIGHT_REAL
```

or VS Code → *Terminal ▸ Run Task… ▸ Upload to Midi Light*.

## Using it

1. **Connect** – pick the board's COM port and click *Connect*. The status dot
   goes green when the board's `READY` banner arrives. Click **PING** any time
   to confirm the round-trip (you'll see a `PONG … (link OK)` line).
2. **Watch** – incoming notes scroll in the console and appear in the *Active
   notes* list while held. The heartbeat (`in/out/err` counters, top-right)
   updates every 2 s.
3. **Record** – click *● Record*, play, click *■ Stop Rec*. Event count shows
   live.
4. **Play back** – click *▶ Play*. Events stream back to the board on a host
   timer and come out the FeatherWing **MIDI OUT** jack. *Speed* scales tempo
   (0.25×–4×). *⏹ All Notes Off* is a panic button.
5. **Save / Load / Export** – store a take as JSON, reload it later, or *Export
   .mid* to a Standard MIDI File you can open in any DAW.

## Debugging

| Control | What it shows |
|---|---|
| **PING** button | Sends `PING`; a `PONG` reply proves the USB link both ways. |
| **Show raw lines** | Echo every line from the board verbatim (incl. `#info`). |
| **Show heartbeats** | Print the periodic `STAT` lines (otherwise just counters). |
| in/out/err counters | Board-side totals: events forwarded, emitted, malformed cmds. |
| Amber dot / "No data" | Port is open but nothing has arrived for 4 s — suspect wiring or baud. |
| **Send** box | Type any protocol command by hand (e.g. `PNON 1 60 100`, `ALLOFF`). |

If the console is silent while you play notes, the MIDI isn't reaching the
board. Set `RAW_MIDI_DUMP`-style checks aside and verify on the firmware side:
open the PlatformIO serial monitor — you should see `EVT …` lines and a `STAT`
every 2 s. No `STAT` at all → wrong COM port. `STAT` but no `EVT` → DIN MIDI
wiring/baud (FeatherWing TX → GPIO44, common ground).

## Serial protocol

Board → PC:

```
READY <name> <version>
#<free text>
EVT <ms> <TYPE> <ch> <d1> <d2>      TYPE: NON NOF CC PB PC AT CAT
STAT <ms> in=<n> out=<n> err=<n>
PONG <ms>
ECHO <text>
```

PC → Board:

```
PING | ECHO <text> | STAT
PNON <ch> <note> <vel>      PNOF <ch> <note> <vel>      PCC <ch> <num> <val>
PPB <ch> <value14>          PPC <ch> <prog>             ALLOFF [ch]
RAW <hexbyte> ...
```

Channels are 1–16. Pitch-bend value is 0–16383 (8192 = center).
