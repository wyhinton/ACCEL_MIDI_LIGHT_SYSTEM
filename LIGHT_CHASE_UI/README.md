# Light Chase UI

React + TypeScript + [easy-peasy](https://easy-peasy.dev/) frontend for configuring light chases and animations, meant to eventually drive the MULTIPLEX_8266 board over USB serial and/or WiFi.

## Status

This is a **frontend-only scaffold**. There is no finalized wire protocol yet:

- `MULTIPLEX_8266/src/main.cpp` currently only understands single ASCII keystrokes over USB serial (`0/1/2/+/-/[/]/b`) and has no WiFi.
- A richer WiFi SoftAP + TCP line protocol (`READY`/`EVT`/`PNON`/etc.) existed briefly on this branch for a different board (`MIDI_NOTE_LIGHT_REAL`) and was removed — see git history (`bcb3b29`) if useful as a reference.

So for now, all "Wired (USB)" and "Wireless (WiFi)" transport options in the UI fall back to a `MockTransport` that simulates a device (see `src/transport/MockTransport.ts`). The app is fully usable end-to-end against the mock.

## Architecture

- `src/types` — domain types: `DeviceChannel`, `ChaseDefinition`/`ChaseStep`, `AnimationPreset`, plus `OutgoingCommand`/`IncomingMessage` sketching a future structured protocol.
- `src/transport` — `Transport` interface (`connect`/`disconnect`/`send`/`onMessage`) and `MockTransport`. `createTransport(kind)` in `src/transport/index.ts` is the single place to swap in real Web Serial / WebSocket implementations later.
- `src/store` — easy-peasy models:
  - `connectionModel` — device connection status, activity log, `connect`/`disconnect`/`sendCommand` thunks. Holds the active `Transport` instance via an injected `transportRef` (see `injections.ts`) so the store never imports a concrete transport directly.
  - `chaseModel` — CRUD for `ChaseDefinition`s and their steps (channels active per step, duration, fade).
  - `animationModel` — CRUD for higher-level `AnimationPreset`s (chase playback, strobe, fade-all, wave, random), with tunable speed/intensity.
  - `uiModel` — active tab only.
- `src/components` — `ConnectionPanel`, `ChasesPanel`/`ChaseEditor`, `AnimationsPanel`, `ChannelChips`.

The default channels and the seeded "Factory Round Robin" chase in `src/store/seedData.ts` mirror the hardcoded 4-step chase currently in `MULTIPLEX_8266/src/main.cpp` (`CHASE_PIN_C -> CHASE_PIN_A -> CHASE_PIN_B -> CHASE_EXT_B`), so the UI reflects real hardware behavior out of the box.

## Running

```bash
npm install
npm run dev
```

## Wiring up a real device later

1. Design a structured command/event protocol (JSON-lines or similar) and add it to `MULTIPLEX_8266/src/main.cpp`, over USB serial and/or WiFi.
2. Implement `Transport` for each real channel (e.g. a `WebSerialTransport` using the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API), a `WebSocketTransport`/TCP-bridge for WiFi).
3. Wire them into `createTransport()` in `src/transport/index.ts` — nothing else in the app needs to change since stores and components only depend on the `Transport` interface.
