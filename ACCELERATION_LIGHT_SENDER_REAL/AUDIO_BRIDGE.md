# Mac → Lights audio bridge

Streams the Mac's output-audio level to the light system so the LED matrices
and PWM lights swell with the music. The level is a **brightness multiplier**
layered on top of the smooth random pulse — it does not replace it.

## Data path

```
Mac app                                    SENDER light board
--------                                   -------------------
capture system audio   BLE                  LightAudioBridge
  -> RMS envelope    ──────────────────▶    write char (1 byte)
  -> smooth 0..255                          -> audioMul = level/255
                                             -> brightness *= audioMul
```

The Mac speaks BLE directly to the SENDER board — no other ESP32s (MIDI board,
receiver) are involved in this path.

## BLE contract (what the firmware exposes)

- Device name: `LightAudioBridge`
- Service UUID: `9a0b0000-1234-4c6e-9b00-1f2e3d4c5b6a`
- Characteristic UUID: `9a0b0001-1234-4c6e-9b00-1f2e3d4c5b6a`
  - Properties: **Write** + **Write Without Response**
  - Payload: **1 byte**, `level` 0–255 (0 = lights off, 255 = full)
  - Rate: stream ~30–60 Hz. Use *write without response* so you don't block per packet.

If no level arrives for `LEVEL_TIMEOUT_MS` (1500 ms) the light boards fall back
to full brightness (multiplier = 1.0), so closing the app just restores the
plain pulse rather than going dark.

## Host script (Python — easiest)

A ready-to-run Python bridge lives in [`host/audio_bridge.py`](../host/audio_bridge.py):

```bash
pip install -r host/requirements.txt
python host/audio_bridge.py --list-devices          # find your input
python host/audio_bridge.py --device "BlackHole"     # capture + stream over BLE
```

It captures an audio **input** device, computes a smoothed RMS envelope, and
writes the level byte over BLE at ~40 Hz using `bleak` + `sounddevice`.

Because Python can't grab macOS *system output* directly, route output through a
virtual loopback (BlackHole) and capture that device.

### Windows

Use `--loopback` to grab system output audio directly via WASAPI loopback — no
Stereo Mix or virtual cable needed (most modern drivers don't expose Stereo Mix
anyway). This uses [`pyaudiowpatch`](https://github.com/s0d3s/PyAudioWPatch),
installed automatically by `uv run` on Windows (or `pip install pyaudiowpatch`).

```bash
python host/audio_bridge.py --loopback                        # default speakers
python host/audio_bridge.py --loopback --device "Headphones"  # a specific output
python host/audio_bridge.py --list-devices --loopback         # list loopback devices
```

If your interface still exposes "Stereo Mix" as a regular input device, the
plain `--device "Stereo Mix"` form (no `--loopback`) also works.

## Mac app outline (Swift — driverless alternative)

1. **Capture system output audio.** Options, newest first:
   - `ScreenCaptureKit` audio capture (macOS 13+) — no driver install.
   - Core Audio process tap / `CATapDescription` (macOS 14.4+) — lowest latency.
   - A virtual loopback device (BlackHole) for older macOS.
2. **Compute level.** Per audio buffer: RMS → exponential moving average with
   separate attack/release (fast attack ~10 ms, slow release ~200–400 ms) →
   normalize/clamp to 0–255. This is the "smooth average."
3. **Send over BLE** with CoreBluetooth:
   - Scan for the service UUID, connect, discover the characteristic.
   - On a ~30–60 Hz timer, `peripheral.writeValue(Data([level]), for: ch, type: .withoutResponse)`.

Keep all the velocity/curve shaping on the device side as today; the Mac only
ships the single smoothed level byte.
