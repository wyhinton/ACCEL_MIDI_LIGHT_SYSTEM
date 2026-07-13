# AUDIO_LEVEL_BRIDGE

Streams the live output level of a Windows audio device (e.g. a USB audio
interface) to the MULTIPLEX_8266 board, where it acts as a **master
brightness scale** on top of whatever mode is running — chase, effects,
test-all, extender channels included.

It reads the Windows endpoint peak meter (`IAudioMeterInformation`, the
Volume Mixer's green bars), so nothing is captured or recorded and any
playing app is picked up. The reading is smoothed (attack/release
envelope), mapped through a dB window and gamma curve, and sent ~60×/s as
2-byte frames.

Entirely optional: the firmware reverts to full brightness one second after
the frames stop, so the lights behave exactly as before when this isn't
running.

## Usage

```
uv run audio_level.py --list                          # find your device's name
uv run audio_level.py --serial-port COM5              # over the USB cable
uv run audio_level.py --udp                           # over Wi-Fi (SoftAP)
uv run audio_level.py --device "USB Audio" --udp --depth 80
```

- **Serial**: the COM port is exclusive, so the script doubles as a
  terminal — the board's output is printed and your keystrokes (the usual
  single-key commands) are forwarded.
- **Wi-Fi**: join the board's SoftAP `MULTIPLEX_LIGHTS` (open network, no
  password), then bare `--udp` targets `192.168.4.1:7777`. The COM port
  stays free for a normal serial monitor. Note the PC loses internet while
  on the SoftAP unless it has a second network adapter.

## How much the audio affects the lights

The board's `audioDepthPct` (0–100%) interpolates between "ignore the
audio" (0) and "brightness fully follows it" (100, the default):

- On the board: `o` / `l` keys raise/lower it 10% per press.
- From here: `--depth 80` sets it over the stream (re-sent every second so
  a rebooting board catches up). Omit `--depth` to leave the board's own
  setting alone.

## Tuning

| Flag | Default | Meaning |
| --- | --- | --- |
| `--attack` / `--release` | 15 / 250 ms | envelope; smaller attack = punchier, larger release = smoother decay |
| `--floor-db` / `--ceil-db` | −45 / −6 | dB window mapped to dark…full |
| `--gamma` | 1.8 | response curve; >1 keeps quiet passages dimmer |
| `--fps` | 60 | meter polls (and frames) per second |

## Wire protocol

Frames are the same over serial (115200 baud, interleaved with key
commands) and UDP (port 7777, one or more frames per packet):

| Frame | Meaning |
| --- | --- |
| `0xAD` `level` | audio level, 0–255 |
| `0xAE` `percent` | set `audioDepthPct`, 0–100 |
