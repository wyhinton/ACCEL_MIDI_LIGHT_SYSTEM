# Art-Net control

Lets QLC+ (or any Art-Net console) control the MULTIPLEX_8266 rig over
Wi-Fi, no USB cable needed. Right now it's a single shared dimmer: one DMX
channel sets the brightness of every relay at once — the 4 local MOSFET
channels and the 3 on the extender board together.

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

## QLC+ setup

1. **Input/Output Manager** → add a plugin output patch on **Art-Net**,
   universe 1 (any universe works — the board doesn't filter by universe
   yet, see [Limitations](#limitations)).
2. Set the output's target IP to the board's address (`192.168.4.1`) for
   unicast, or leave it on the interface's broadcast address
   (`192.168.4.255`) — both work.
3. In the workspace, add a **Generic Dimmer** (or Simple Desk channel) on
   **channel 1** of that universe. Its slider now fades every relay
   together, 0–255 mapping to fully off–fully on.

Sending Art-Net immediately takes over from whatever mode the board was
running (chase, effects, performance auto-switching) — the same as pressing
`t` at the serial console.

## Status

The board's periodic `[STATUS]` serial line reports Art-Net alongside the
extender link, SoftAP, audio, and MIDI status:

```
[STATUS] extender: UP (heartbeat 0.2s ago) | AP: 1 station(s) | audio: none | artnet: active (brightness 782/1023, 0.1s ago) | MIDI: no frames yet
```

`artnet: none` means no ArtDMX packet has arrived yet since boot.

## Limitations

This is a first, basic version:

- **No universe filtering** — channel 1 of whatever universe QLC+ sends is
  read, regardless of the Net/SubUni bytes in the packet.
- **Single channel** — only DMX channel 1 is read; there's no per-relay
  addressing yet.
- **No timeout hand-back** — if the Art-Net stream stops (QLC+ closes, the
  PC disconnects from the SoftAP), the relays hold their last commanded
  brightness rather than reverting to the chase/effects/performance mode.
- **No ArtPoll reply** — the board won't show up in QLC+'s Art-Net node
  discovery; you have to enter its IP manually.

## Wire protocol

Standard [Art-Net](https://art-net.org.uk/) `ArtDmx` (`OpDmx`, opcode
`0x5000`) packets: 8-byte ID `"Art-Net\0"`, little-endian OpCode, then the
usual Sequence/Physical/SubUni/Net/Length fields, then DMX data. The board
validates the header and opcode, then reads only the first data byte
(channel 1):

| Bytes | Field | Board's use |
| --- | --- | --- |
| 0–7 | `"Art-Net\0"` | must match, else packet ignored |
| 8–9 | OpCode (LE) | must be `0x5000` (`OpDmx`), else ignored |
| 10–17 | ProtVer / Sequence / Physical / SubUni / Net / Length | not read |
| 18 | DMX channel 1 | shared relay brightness, 0–255 |
| 19+ | DMX channels 2–512 | not read |
