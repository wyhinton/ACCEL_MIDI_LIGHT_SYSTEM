# Branches

This repo is a workbench, not a product: each branch is a **snapshot of a
working rig** — a particular set of boards, wired a particular way, running a
particular control scheme. Branches are mostly *not* merged back into `master`,
and several replace whole projects rather than adding to them. Pick the branch
that matches the hardware on the bench.

This file is kept on every branch, so it also describes branches you are not
currently on. It is the only file that is intentionally identical everywhere.

## Two hardware eras

The branches split into two families that share almost no code:

| Era | Boards | Transport | Projects |
| --- | --- | --- | --- |
| **ESP32-S3** (Jun–Jul 2026) | Seeed XIAO / Waveshare ESP32-S3 with 8×8 WS2812 matrices, QMI8658C IMU, MIDI FeatherWing | ESP-NOW between boards, BLE / USB serial to a host PC | `ACCELERATION_LIGHT_SENDER_REAL`, `ACCLERATION_LIGHT_RECIEVER_REAL`, `MIDI_NOTE_LIGHT_REAL`, `host/` |
| **ESP8266** (Jul–Sep 2026) | NodeMCU v2 boards driving MOSFET switch modules and a PCA9685 | SoftwareSerial link + Wi-Fi SoftAP, Art-Net (DMX-over-UDP) from QLC+ | `MULTIPLEX_8266`, `MOSFET_EXTENDER_8266`, `RELAY_PWM_8266`, `AUDIO_LEVEL_BRIDGE` |

The ESP8266 branches deleted the ESP32-S3 projects outright — the two families
are alternatives, not layers.

## Quick index

| Branch | Era | In one line |
| --- | --- | --- |
| [`master`](#master) | ESP32-S3 | Baseline: IMU tilt/impact sender → ESP-NOW → relay receiver, plus a MIDI note-flash board |
| [`merge-startup-letter-and-tasks`](#merge-startup-letter-and-tasks) | ESP32-S3 | Identical to `master` — a leftover merge branch |
| [`bridge_show`](#bridge_show) | ESP32-S3 | `master` + the MIDI board flashes a **relay only**, matrix reduced to an identity banner |
| [`soft_random`](#soft_random) | ESP32-S3 | Crash detection ripped out; lights breathe on a smooth random pulse, scaled by Mac audio over BLE |
| [`audio_controlled_brightness_pc`](#audio_controlled_brightness_pc) | ESP32-S3 | Audio→brightness bridge talking BLE **straight to the sender**, no ESP-NOW hop |
| [`jerk_crash_midi_ble_pc`](#jerk_crash_midi_ble_pc) | ESP32-S3 | Sender emits BLE-MIDI on jerk/crash; a Python bridge feeds it into a DAW via loopMIDI |
| [`mac_pc_wifi_login`](#mac_pc_wifi_login) | side quest | Unrelated: two ESP32s bridge Wi-Fi credentials from a Mac to a headless PC via USB-HID keystrokes |
| [`midi_rec_for_recording`](#midi_rec_for_recording) | ESP32-S3 | MIDI board becomes a pure MIDI recorder/player with a tkinter desktop UI |
| [`8266_multiplex_sequenced_lights`](#8266_multiplex_sequenced_lights) | ESP8266 | First ESP8266 rig: 7 MOSFET channels, serial-key control, QLC+ effect ports, Windows audio bridge |
| [`relay_artnet_control`](#relay_artnet_control--pwm_extra_multiplex_initial_test) | ESP8266 | Serial control replaced by a 2-channel Art-Net fixture (brightness + pattern select) |
| [`PWM_EXTRA_MULTIPLEX_INITIAL_TEST`](#relay_artnet_control--pwm_extra_multiplex_initial_test) | ESP8266 | Same commit as `relay_artnet_control` — a duplicate label |
| [`RELAY_PWM_MULTIPLEX_CHANNELS`](#relay_pwm_multiplex_channels) | ESP8266 | **Current work.** 15-channel Art-Net fixture; 8 extra PWM channels on a wireless PCA9685 board |

---

## ESP32-S3 branches

### `master`

The common ancestor everything else grew from. Three PlatformIO projects:

- **`ACCELERATION_LIGHT_SENDER_REAL`** — Waveshare ESP32-S3 with the QMI8658C
  IMU. Moves a dot on the 8×8 matrix by tilt; when acceleration crosses a
  threshold the matrix flashes white and an `ImpactMessage` (`seq`, `jerk`,
  `accelMag`) goes out over ESP-NOW broadcast. Also emits BLE-MIDI.
- **`ACCLERATION_LIGHT_RECIEVER_REAL`** — XIAO ESP32-S3. Listens for those
  impact messages and pulses a relay driving a real light. Keeps a HELLO/ACK
  handshake with the sender so both ends know the link is alive.
- **`MIDI_NOTE_LIGHT_REAL`** — XIAO ESP32-S3 + MIDI FeatherWing. Note On →
  the whole matrix flashes white, velocity scaling brightness and duration.

Also carries the startup identity letter shown on each board's matrix at boot
and the VS Code upload tasks. Note: `origin/master` is ahead of local `master`
— it drops some dead code from the two accel projects and reworks
`.vscode/tasks.json`.

### `merge-startup-letter-and-tasks`

Points at the exact same commit as local `master`. Nothing distinguishes it;
it is a merge branch that was never cleaned up.

### `bridge_show`

`master` plus two commits on `MIDI_NOTE_LIGHT_REAL` only. The MIDI board gains
a relay on GPIO 2 and stops pulsing the matrix: a Note On on channel 1 closes
the relay for the flash window, while the matrix only shows an `M` identity
banner at boot and a dim green power dot at rest. This is the "real lamp, no
LED matrix in the audience's face" show configuration.

### `soft_random`

A significant rewrite of the sender (~1000 lines changed). IMU crash detection
is **removed entirely** — the matrix and the PWM light on GPIO 2 just breathe,
easing between random brightness targets over random durations. MIDI-triggered
flashes from the MIDI board still pop on top over ESP-NOW.

Adds `host/audio_bridge.py` and `MIDI_NOTE_LIGHT_REAL/AUDIO_BRIDGE.md`: a Mac
captures its own output level, smooths it to an RMS envelope, and writes a
single 0–255 byte over BLE to the MIDI board (`LightAudioBridge`), which
rebroadcasts it across the ESP-NOW mesh as a **brightness multiplier** on top
of the pulse. If no level arrives for 1.5 s the boards fall back to full
brightness, so quitting the app restores the plain pulse instead of going dark.
`origin/soft_random` is two commits ahead of the local branch.

### `audio_controlled_brightness_pc`

*(remote-only — `origin/audio_controlled_brightness_pc`)*

Branches off the `soft_random` line and simplifies the audio path: the Mac
speaks BLE **directly to the sender board**, with no MIDI board or receiver in
the loop. `AUDIO_BRIDGE.md` moves to `ACCELERATION_LIGHT_SENDER_REAL/`. The
receiver and MIDI projects are gone from this branch; `FIELD_REC_TAIWAN.wav`
/ `.mp3` are committed as test material.

### `jerk_crash_midi_ble_pc`

*(remote-only — `origin/jerk_crash_midi_ble_pc`)*

Continues from `audio_controlled_brightness_pc`. The sender detects jerk/crash
again and publishes it as **BLE-MIDI** from a service named `AccelLight`.
`host/midi_ble_bridge.py` (a `uv run` script with PEP 723 inline deps) connects
over BLE, decodes the notifications, and forwards each message into a named
virtual MIDI port so any DAW sees the rig as a normal MIDI input — on Windows
via a pre-created loopMIDI port, since Windows has no API for creating one.
Ships a status UI, `start_midi_bridge.bat`, and Ableton material
(`IMPACT_RACK_TAIWAN_1.adg`, `TAIWAN_IMPACT_PACK_TEST.alp`).

### `mac_pc_wifi_login`

*(remote-only — `origin/mac_pc_wifi_login`)*

A side quest unrelated to lighting. Two new projects,
`WIFI_LOGIN_MAC_SENDER` and `WIFI_LOGIN_PC_RECEIVER`, use a pair of ESP32-S3s
to get a headless auto-login Windows PC onto Wi-Fi when the ESP32 in its USB
port is the *only* way in. The Mac sends SSID/password over USB serial →
ESP-NOW → the PC-side board, which enumerates as a USB HID keyboard and types a
`powershell -EncodedCommand` bootstrap into `Win+R`; that self-contained script
builds a WLAN profile via `netsh`. The result comes back over the board's COM
port as `RESULT:OK` / `RESULT:FAIL:<detail>`.

Caution: this branch has `MULTIPLEX_8266/.pio/build/**` object files committed.

### `midi_rec_for_recording`

Repurposes `MIDI_NOTE_LIGHT_REAL` as a **MIDI recorder** — on this branch that
firmware does nothing with lights, relay, or ESP-NOW and is purely a MIDI ⇄ host
bridge. It speaks two transports simultaneously: USB serial at 115200 and a
Wi-Fi SoftAP with a TCP server on port 5000. Adds `MIDI_RECORDER_UI/`, a tkinter
desktop app (only pip dep: `pyserial`) that records incoming notes and plays
them back out the FeatherWing's MIDI OUT jack, with a **Via:** selector for the
transport. The accel sender/receiver projects are still present but untouched.

---

## ESP8266 branches

All three drop the ESP32-S3 projects and build the MOSFET rig instead. The
shared skeleton across them:

- **`MULTIPLEX_8266`** — primary NodeMCU. Drives 4 MOSFET channels directly
  (GPIO 4/5/0/15) and hosts a WPA2 SoftAP, `MULTIPLEX_LIGHTS` @ `192.168.4.1`
  (password `LIGHTS123`).
- **`MOSFET_EXTENDER_8266`** — second NodeMCU driving 3 more MOSFETs, fed over
  a crossed SoftwareSerial link on GPIO 12/14 at 9600 baud. Framing is
  sync-byte based: `0xAA` per-channel on/off, `0xAB` all-channel brightness,
  `0xAC` per-channel raw duty, so the link resynchronizes after line noise
  instead of misreading a stray byte as a command.
- **`src/effects.h`** — hand-ports of QLC+'s own RGB matrix scripts
  (`onebyone.js`, `fill.js`, `fillunfill.js`, `evenodd.js`, `strobe.js`, from
  qlcplus commit `25c41450`, Apache-2.0 preserved) treating the 7 channels as a
  width×1 pixel strip, each pixel collapsing to a PWM duty.

### `8266_multiplex_sequenced_lights`

The first ESP8266 rig. Control is entirely over **USB serial keypresses**:
polarity, PWM fade vs instant switching, fade in/out times, chase step length,
blackout, test-all, effects mode with next/previous effect, a performance mode
that hops to a random effect every 30–60 s, and an `m` calibration mode that
lights each channel alone so the physical left-to-right light order can be
recorded to EEPROM.

Uniquely, this branch still carries `MIDI_NOTE_LIGHT_REAL` alongside the 8266
projects, and adds **`AUDIO_LEVEL_BRIDGE/audio_level.py`**: it reads the Windows
endpoint peak meter (`IAudioMeterInformation` — the Volume Mixer's green bars,
so nothing is captured or recorded) and streams a smoothed, dB-windowed,
gamma-curved level at ~60 Hz over USB serial or UDP as a **master brightness
scale** over whatever mode is running. The firmware reverts to full brightness
one second after the frames stop, so it is entirely optional.

### `relay_artnet_control` / `PWM_EXTRA_MULTIPLEX_INITIAL_TEST`

**These are the same commit** (`bf740ed`) under two names.

The serial command interface is removed — USB becomes log output only — and
replaced by **Art-Net (DMX-over-UDP) on port 6454**, so QLC+ drives the rig over
Wi-Fi with no cable. It is a **2-channel fixture**: channel 1 is one shared
brightness for all 7 relays, channel 2 selects an animated pattern from
`effects.h` (0 = flat fill). Adds `MULTIPLEX_8266/ARTNET_CONTROL.md` and
`connect_wifi.py`, a `uv run` helper that joins the SoftAP on Windows and pings
the board to confirm it is reachable.

### `RELAY_PWM_MULTIPLEX_CHANNELS`

**Current branch / newest work** (Sep 2026). Adds a third board:

- **`RELAY_PWM_8266`** — NodeMCU with a PCA9685 16-channel PWM driver on I²C
  (GPIO 4/5) and an SSD1306 OLED. Notably it has **no wire to the primary at
  all**: it joins `MULTIPLEX_LIGHTS` as a Wi-Fi station — the same WPA2 network
  QLC+ is on — and the two exchange duty commands and heartbeats as UDP
  datagrams. It does not speak Art-Net itself.

The fixture grows to **15 Art-Net channels, one DMX channel per lamp** (channel
N = the lamp at physical position N−1 via the light-order map), each an
independent 0–255 brightness: channels 1–7 are the 4 local MOSFETs plus the 3
on the extender, channels 8–15 are the PCA9685 outputs. The board deliberately
hides which lamp is driven by which kind of hardware. The `effects.h` pattern
engine is still compiled in but **is not wired to Art-Net** — kept for a later
rewire rather than deleted.

Two hardware caveats are recorded in the code: the OLED is flagged
`OLED_ATTACHED = false` because its VCC/ground wiring was implicated in a
heat/power problem when the 12 V supply was also connected, and a failed
`display.begin()` no longer blocks `setup()` — it used to spin forever, which
looked exactly like a Wi-Fi failure from the primary's side (still associated,
never a heartbeat). The tip commit, `CHANGE TO FIX PACKET DROP`, reworks that
UDP link on both boards.

`origin/RELAY_PWM_MULTIPLEX_CHANNELS` is one commit ahead of local: it adds a
root `.gitignore`, `.vscode/scripts/pio.bat` / `pio.sh` wrappers, reworked
tasks, and removes a committed `__pycache__`.
