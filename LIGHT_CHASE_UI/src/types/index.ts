/**
 * Domain types shared between the stores, transport layer, and UI.
 *
 * There is no finalized wire protocol with the firmware yet (MULTIPLEX_8266
 * currently only understands single ASCII keystrokes over USB serial, and has
 * no WiFi). These types describe the shape the app works with internally;
 * OutgoingCommand/IncomingMessage double as a sketch of a future structured
 * protocol that a real Transport implementation would translate to/from.
 */

export type TransportKind = 'wired' | 'wireless' | 'mock';

export interface DeviceChannel {
  id: string;
  label: string;
  /** Descriptive only (e.g. "Local GPIO4") until a real protocol assigns channel numbers. */
  physicalRef: string;
}

export interface ChaseStep {
  id: string;
  activeChannelIds: string[];
  durationMs: number;
  fade: boolean;
  fadeDurationMs: number;
}

export interface ChaseDefinition {
  id: string;
  name: string;
  loop: boolean;
  steps: ChaseStep[];
}

export type AnimationKind = 'chase' | 'strobe' | 'fadeAll' | 'wave' | 'random';

export interface AnimationPreset {
  id: string;
  name: string;
  kind: AnimationKind;
  /** Only meaningful when kind === 'chase' — references a ChaseDefinition id. */
  chaseId?: string;
  params: {
    speedMs: number;
    intensity: number;
  };
}

export interface LayoutCell {
  id: string;
  row: number;
  col: number;
  channelId: string | null;
}

export interface DeviceStatus {
  connected: boolean;
  connecting: boolean;
  transportKind: TransportKind | null;
  lastError?: string;
  firmwareVersion?: string;
  linkOk?: boolean;
}

export type OutgoingCommand =
  | { type: 'applyChase'; chase: ChaseDefinition }
  | { type: 'applyAnimation'; animation: AnimationPreset }
  | { type: 'setBlackout'; value: boolean }
  | { type: 'stop' }
  | { type: 'ping' };

export type IncomingMessage =
  | { type: 'ready'; firmwareVersion: string }
  | { type: 'ack'; command: OutgoingCommand['type'] }
  | { type: 'status'; linkOk: boolean }
  | { type: 'log'; text: string }
  | { type: 'pong' };
