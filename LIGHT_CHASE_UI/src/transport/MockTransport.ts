import type { IncomingMessage, OutgoingCommand } from '../types';
import type { MessageListener, Transport } from './Transport';

const CONNECT_DELAY_MS = 500;
const SEND_DELAY_MS = 120;
const HEARTBEAT_INTERVAL_MS = 4000;

/** Simulates a device connection so the UI/stores are fully usable with no hardware attached. */
export class MockTransport implements Transport {
  readonly kind = 'mock' as const;

  private connected = false;
  private listeners = new Set<MessageListener>();
  private heartbeatTimer: ReturnType<typeof setInterval> | undefined;

  isConnected(): boolean {
    return this.connected;
  }

  async connect(): Promise<void> {
    await delay(CONNECT_DELAY_MS);
    this.connected = true;
    this.emit({ type: 'ready', firmwareVersion: 'mock-0.1.0' });
    this.heartbeatTimer = setInterval(() => {
      this.emit({ type: 'status', linkOk: true });
    }, HEARTBEAT_INTERVAL_MS);
  }

  async disconnect(): Promise<void> {
    this.connected = false;
    if (this.heartbeatTimer !== undefined) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  async send(command: OutgoingCommand): Promise<void> {
    if (!this.connected) {
      throw new Error('Cannot send: not connected');
    }
    await delay(SEND_DELAY_MS);
    this.emit({ type: 'log', text: describe(command) });
    this.emit({ type: 'ack', command: command.type });
  }

  onMessage(listener: MessageListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private emit(message: IncomingMessage): void {
    this.listeners.forEach((listener) => listener(message));
  }
}

function describe(command: OutgoingCommand): string {
  switch (command.type) {
    case 'applyChase':
      return `Applied chase "${command.chase.name}" (${command.chase.steps.length} steps)`;
    case 'applyAnimation':
      return `Applied animation "${command.animation.name}" (${command.animation.kind})`;
    case 'setBlackout':
      return `Blackout ${command.value ? 'on' : 'off'}`;
    case 'stop':
      return 'Stopped';
    case 'ping':
      return 'Ping';
  }
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
