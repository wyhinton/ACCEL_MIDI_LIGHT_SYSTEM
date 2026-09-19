# Art-Net control

Lets MagicQ, QLC+, or any Art-Net console control the MULTIPLEX_8266 rig
over Wi-Fi, no USB cable needed. It's a 15-channel fixture, one DMX channel
per relay: channel N is the lamp at physical position N (1 = first lamp in
the row, matching the board's light-order map), 0–255 mapping to fully
off–fully on. The 4 local MOSFET channels, the 3 on the extender board, and
the 8 PCA9685 PWM outputs on the RELAY_PWM_8266 board are all addressed
identically — the board hides which is which.

The board also answers `ArtPoll` and accepts `ArtAddress`, so it's a
standard, discoverable Art-Net node: it'll show up in the console's own node
list (MagicQ's Setup / View Nodes, QLC+'s equivalent), and its
Net/Sub-Net/Universe address — the standard Art-Net "start address" — can be
set from there instead of a firmware reflash. See
[Setting the node's address](#setting-the-nodes-address) below.

This is the board's only control surface: there's no serial command
interface anymore, so USB is log output only.

The firmware also has a pattern engine (animated chases/fills ported from
QLC+'s own RGB scripts, see `effects.h`) but it isn't wired to Art-Net right
now — see `main.cpp`'s "Pattern selection" section. It's kept in place, not
deleted, for a later rewire.

## Connecting

The board creates the same SoftAP used by the
[audio bridge](../AUDIO_LEVEL_BRIDGE/README.md):

| | |
| --- | --- |
| SSID | `MULTIPLEX_LIGHTS` |
| Password | `LIGHTS123` (WPA2) |
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
   universe 1 (the board defaults to Net 0 / Sub-Net 0 / Universe 0, which
   most consoles including QLC+ and MagicQ label "Universe 1" in their
   1-based UI — see [Setting the node's address](#setting-the-nodes-address)
   to point it at a different universe instead).
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

## MagicQ setup

1. **View** → **View Outputs** (or **Setup** → **DMX I/O**, depending on
   version) → add an **Art-Net** output on universe 1, targeting broadcast
   or the board's IP — same as the QLC+ steps above.
2. Patch a 15-channel dimmer fixture (or 15 single-channel dimmers) onto
   that universe, starting at channel 1. As with QLC+, channel 1 is the
   first lamp in the row.

MagicQ's **Setup → View Nodes** screen should list the board once it's
received at least one `ArtPoll` (MagicQ sends these periodically on its
own) — see the next section if you want to reassign its universe from
there instead of the patch.

## Setting the node's address

The board is a standard Art-Net node: it answers `ArtPoll` with an
`ArtPollReply` describing itself (`MULTIPLEX_8266`, its current
Net/Sub-Net/Universe, MAC, etc.), so it shows up in the console's own node
list — MagicQ's **Setup → View Nodes**, QLC+'s **Art-Net node list** under
the Art-Net plugin config. From there, setting the node's universe sends it
an `ArtAddress` packet, which the board applies immediately and saves to
flash (EEPROM), so it comes back up on the same address after a power
cycle.

This sets which 512-channel *universe* the board listens to — the
standard Art-Net "start address" for a node — not an arbitrary offset
*within* a universe: the board always reads channels 1–15 of whichever
universe it's addressed to, it can't pick out channels 100–114 from the
middle of one. So if this fixture needs to share a universe with other gear
at a non-1 start address, that won't work here — give it its own dedicated
universe (via ArtAddress, above) and patch it starting at channel 1 on that
universe instead.

The current address is echoed in the boot log and the periodic `[STATUS]`
line (see below) as `addr: <net>/<sub-net>/<universe>`.

## Status

The board's periodic `[STATUS]` serial line reports Art-Net alongside both
satellite links (the wired extender and the wireless RELAY_PWM_8266), SoftAP,
and audio status:

```
[STATUS] Extender: UP (heartbeat 0.2s ago) | RELAY_PWM_8266: UP (heartbeat 0.1s ago) | AP: 1 station(s) | addr: 0/0/0 | audio: none | artnet: active (levels 0 255 128 0 0 64 0 0 0 0 0 0 0 0 0, 0.1s ago)
```

`addr: <net>/<sub-net>/<universe>` is the node's current Art-Net address
(see [Setting the node's address](#setting-the-nodes-address)). The 15
numbers after `levels` are channels 1–15 in order. `artnet: none` means no
ArtDMX packet matching that address has arrived yet since boot (the fixture
boots dark, every relay off, until the console sends something).
`RELAY_PWM_8266: DOWN` means that board hasn't joined the SoftAP yet or has
dropped off Wi-Fi — channels 8–15 just hold their last commanded state on
that board rather than affecting this one. Once it (re)joins, this board
resends the current state of all 8 of its channels so it catches up instead
of staying dark.

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

- **No in-universe start-channel offset** — the fixture always occupies
  channels 1–15 of its configured universe; there's no way to place it at,
  say, channels 100–114 of a universe shared with other fixtures (see
  [Setting the node's address](#setting-the-nodes-address)).
- **No built-in patterns yet** — the ported QLC+ effects (chases, fills,
  strobe) exist in the firmware but aren't reachable over Art-Net right
  now; every channel is a plain dimmer.
- **No timeout hand-back** — if the Art-Net stream stops (the console
  closes, the PC disconnects from the SoftAP), the relays hold their last
  commanded state rather than reverting to anything else.
- **No fallback control path** — with the serial command interface removed,
  if the SoftAP fails to start there's no way to reach the board short of a
  power cycle.

## Wire protocol

Three [Art-Net](https://art-net.org.uk/) opcodes are handled, all on UDP
port 6454, all sharing the standard 8-byte ID `"Art-Net\0"` + little-endian
OpCode header:

### `ArtDmx` (`OpDmx`, `0x5000`)

The board validates the header and opcode, checks the packet's
Net/Sub-Net/Universe against the node's configured address (see
[Setting the node's address](#setting-the-nodes-address); packets for any
other universe are ignored), then reads only the first 15 data bytes
(channels 1–15). A packet shorter than 15 channels is still applied, with
whichever trailing channels are missing treated as 0 (off):

| Bytes | Field | Board's use |
| --- | --- | --- |
| 0–7 | `"Art-Net\0"` | must match, else packet ignored |
| 8–9 | OpCode (LE) | must be `0x5000` (`OpDmx`), else ignored |
| 10–13 | ProtVer / Sequence / Physical | not read |
| 14 | SubUni (Sub-Net hi nibble, Universe lo nibble) | must match the node's address |
| 15 | Net | must match the node's address (top bit ignored) |
| 16–17 | Length | not read |
| 18–32 | DMX channels 1–15 | per-relay brightness, 0–255 each |
| 33+ | DMX channels 16–512 | not read |

### `ArtPoll` (`0x2000`) → `ArtPollReply` (`0x2100`)

Any `ArtPoll` (from any controller doing node discovery, e.g. MagicQ's
periodic poll or QLC+ scanning for nodes) gets a broadcast `ArtPollReply`
back describing the node: IP, `MULTIPLEX_8266` short/long name, current
NetSwitch/SubSwitch/SwOut (its address), one output-type port, and MAC.
Nothing in the poll packet itself is inspected — every poll gets the same
reply.

### `ArtAddress` (`0x6000`)

Sets the node's address: `NetSwitch`/`SubSwitch` apply only when their
top "program" bit (`0x80`) is set, and `SwOut[0] == 0x7F` means "leave the
universe unchanged" — the usual Art-Net sentinels for "this field wasn't
touched in the console's address dialog." A change is saved to EEPROM
immediately and followed by a fresh `ArtPollReply` so the console sees the
update take effect. `ShortName`/`LongName`/`BindIndex`/`Command` and
everything else in the packet are accepted but not read.

Channels 8–15 aren't driven directly off the `ArtDmx` packet: the board re-streams
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
