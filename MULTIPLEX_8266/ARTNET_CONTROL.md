# Art-Net control

Lets QLC+ (or any Art-Net console) control the MULTIPLEX_8266 rig over
Wi-Fi, no USB cable needed. It's a 15-channel fixture, one DMX channel per
relay: channel N is the lamp at physical position N (1 = first lamp in the
row, matching the board's light-order map), 0–255 mapping to fully
off–fully on. The 4 local MOSFET channels, the 3 on the extender board, and
the 8 PCA9685 PWM outputs on the RELAY_PWM_8266 board are all addressed
identically — the board hides which is which.

This is the board's only control surface: there's no serial command
interface anymore, so USB is log output only.

The firmware also has a pattern engine (animated chases/fills ported from
QLC+'s own RGB scripts, see `effects.h`) but it isn't wired to Art-Net right
now — see `main.cpp`'s "Pattern selection" section. It's kept in place, not
deleted, for a later rewire.

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
3. In the workspace, add a 15-channel fixture on that universe — a **Generic
   Dimmer** with 15 **Intensity** capabilities, or 15 separate single-channel
   dimmers, whichever's easier to build. Each channel's slider fades one
   relay: channel 1 = the first lamp in the row, channel 2 = the second,
   and so on, in the board's calibrated physical order regardless of which
   pin (or which board) actually drives each one.

Sending Art-Net takes effect immediately: each incoming packet retargets
the fade engine right away, per channel.

## Status

The board's periodic `[STATUS]` serial line reports Art-Net alongside both
satellite links (the wired extender and the wireless RELAY_PWM_8266), SoftAP,
and audio status:

```
[STATUS] Extender: UP (heartbeat 0.2s ago) | RELAY_PWM_8266: UP (heartbeat 0.1s ago) | AP: 1 station(s) | audio: none | artnet: active (levels 0 255 128 0 0 64 0 0 0 0 0 0 0 0 0, 0.1s ago)
```

The 15 numbers after `levels` are channels 1–15 in order. `artnet: none`
means no ArtDMX packet has arrived yet since boot (the fixture boots dark,
every relay off, until QLC+ sends something). `RELAY_PWM_8266: DOWN` means
that board hasn't joined the SoftAP yet or has dropped off Wi-Fi — channels
8–15 just hold their last commanded state on that board rather than
affecting this one. Once it (re)joins, this board resends the current state
of all 8 of its channels so it catches up instead of staying dark.

## Debug log over Wi-Fi

Everything the board prints over USB serial is also mirrored to a plain TCP
port, so you can watch the log from whatever machine is joined to the SoftAP
without a USB cable:

```
telnet 192.168.4.1 23
```

Only one Telnet client is served at a time — connecting a second one drops
the first.

## Limitations

This is a first, basic version:

- **No universe filtering** — channels 1–15 of whatever universe QLC+ sends
  are read, regardless of the Net/SubUni bytes in the packet.
- **No built-in patterns yet** — the ported QLC+ effects (chases, fills,
  strobe) exist in the firmware but aren't reachable over Art-Net right
  now; every channel is a plain dimmer.
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
validates the header and opcode, then reads only the first 15 data bytes
(channels 1–15); a packet shorter than 15 channels is still applied, with
whichever trailing channels are missing treated as 0 (off):

| Bytes | Field | Board's use |
| --- | --- | --- |
| 0–7 | `"Art-Net\0"` | must match, else packet ignored |
| 8–9 | OpCode (LE) | must be `0x5000` (`OpDmx`), else ignored |
| 10–17 | ProtVer / Sequence / Physical / SubUni / Net / Length | not read |
| 18–32 | DMX channels 1–15 | per-relay brightness, 0–255 each |
| 33+ | DMX channels 16–512 | not read |

Channels 8–15 aren't driven directly off this packet: the board re-streams
them to RELAY_PWM_8266 as `[0xAC sync][channel 0-7][duty 0-255]` UDP
datagrams (the same framing already used for the extender's channels 4–7,
just sent over Wi-Fi instead of a wire), so RELAY_PWM_8266 never has to
parse Art-Net itself. RELAY_PWM_8266 joins this board's SoftAP as a Wi-Fi
station rather than being wired in -- it has no fixed IP of its own (DHCP
hands one out), so this board learns it from the source address of
RELAY_PWM_8266's own heartbeat packets:

| Port | Direction | Contents |
| --- | --- | --- |
| `7778`/udp | RELAY_PWM_8266 → this board | heartbeat (link-up signal only; content ignored) |
| `7779`/udp | this board → RELAY_PWM_8266 | `[0xAC][channel 0-7][duty 0-255]` |

See RELAY_PWM_8266's `main.cpp` for the receiving side.
