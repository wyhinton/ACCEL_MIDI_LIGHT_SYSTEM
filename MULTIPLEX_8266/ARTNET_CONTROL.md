# Art-Net control

Lets QLC+ (or any Art-Net console) control the MULTIPLEX_8266 rig over
Wi-Fi, no USB cable needed. It's a 2-channel fixture:

- **Channel 1 — brightness.** Fades every relay together, the 4 local
  MOSFET channels and the 3 on the extender board alike, 0–255 mapping to
  fully off–fully on.
- **Channel 2 — pattern.** Picks one of the animated patterns ported from
  QLC+'s own RGB scripts (see `effects.h`), or **0 for no pattern** — a
  flat fill at channel 1's brightness, the fixture's default state.

This is the board's only control surface: there's no serial command
interface anymore, so USB is log output only.

## Connecting

The board creates the same open SoftAP used by the
[audio bridge](../AUDIO_LEVEL_BRIDGE/README.md):

| | |
| --- | --- |
| SSID | `MULTIPLEX_LIGHTS` (open, no password) |
| Board IP | `192.168.4.1` |
| Art-Net port | `6454` (UDP, the standard Art-Net port) |

Join that network from the machine running QLC+. The PC loses internet
access while connected unless it has a second network adapter.

On Windows, [connect_wifi.py](connect_wifi.py) automates joining (and
pings the board afterward to confirm it's reachable):

```
uv run connect_wifi.py
```

## QLC+ setup

1. **Input/Output Manager** → add a plugin output patch on **Art-Net**,
   universe 1 (any universe works — the board doesn't filter by universe
   yet, see [Limitations](#limitations)).
2. Set the output's target IP to the board's address (`192.168.4.1`) for
   unicast, or leave it on the interface's broadcast address
   (`192.168.4.255`) — both work.
3. In the workspace, add a 2-channel **Generic Dimmer** fixture on that
   universe: channel 1 = **Intensity**, channel 2 = a **Pattern**/**Gobo**
   capability (any capability type works — the board just reads the raw
   0–255 value). Channel 1's slider fades every relay together; channel 2
   picks the pattern:

   | DMX value | Pattern |
   | --- | --- |
   | 0–36 | None (flat fill at channel 1's brightness) |
   | 37–73 | One By One |
   | 74–109 | Fill |
   | 110–146 | Fill Reversed |
   | 147–182 | Fill Unfill |
   | 183–219 | Even/Odd |
   | 220–255 | Strobe |

   (256 values split into 7 equal buckets — one "none" plus the 6 effects
   in `effects.h`'s `EFFECTS[]` order. Adding/removing an effect there
   shifts these boundaries.)

Sending Art-Net takes effect immediately: each incoming packet retargets
the fade engine right away, and a pattern change always restarts that
pattern from its first frame.

## Status

The board's periodic `[STATUS]` serial line reports Art-Net alongside the
extender link, SoftAP, and audio status:

```
[STATUS] extender: UP (heartbeat 0.2s ago) | AP: 1 station(s) | audio: none | artnet: active (brightness 200/255, pattern 3/6, 0.1s ago)
```

`artnet: none` means no ArtDMX packet has arrived yet since boot (the
fixture boots dark, pattern 0/none, until QLC+ sends something).

## Limitations

This is a first, basic version:

- **No universe filtering** — channels 1–2 of whatever universe QLC+ sends
  are read, regardless of the Net/SubUni bytes in the packet.
- **No per-relay addressing** — brightness is shared across all 7 relays;
  there's no way to address one individually.
- **Fixed pattern speed** — animated patterns always step every 500ms
  (`PATTERN_STEP_MS` in `main.cpp`); there's no speed channel yet.
- **No timeout hand-back** — if the Art-Net stream stops (QLC+ closes, the
  PC disconnects from the SoftAP), the relays hold their last commanded
  state rather than reverting to anything else.
- **No ArtPoll reply** — the board won't show up in QLC+'s Art-Net node
  discovery; you have to enter its IP manually.
- **No fallback control path** — with the serial command interface removed,
  if the SoftAP fails to start there's no way to reach the board short of a
  power cycle.

## Wire protocol

Standard [Art-Net](https://art-net.org.uk/) `ArtDmx` (`OpDmx`, opcode
`0x5000`) packets: 8-byte ID `"Art-Net\0"`, little-endian OpCode, then the
usual Sequence/Physical/SubUni/Net/Length fields, then DMX data. The board
validates the header and opcode, then reads only the first two data bytes
(channels 1–2); a packet with just channel 1 present is still applied, with
channel 2 (pattern) treated as 0:

| Bytes | Field | Board's use |
| --- | --- | --- |
| 0–7 | `"Art-Net\0"` | must match, else packet ignored |
| 8–9 | OpCode (LE) | must be `0x5000` (`OpDmx`), else ignored |
| 10–17 | ProtVer / Sequence / Physical / SubUni / Net / Length | not read |
| 18 | DMX channel 1 | shared relay brightness, 0–255 |
| 19 | DMX channel 2 | pattern select, 0–255 (bucketed, see above) |
| 20+ | DMX channels 3–512 | not read |
