import type { IncomingMessage, OutgoingCommand, TransportKind } from '../types';

export type MessageListener = (message: IncomingMessage) => void;

/**
 * Everything the stores need from a device connection. 'wired' and 'wireless'
 * are placeholders today (see createTransport in ./index.ts) — real
 * implementations would wrap the Web Serial API and a WebSocket/TCP bridge
 * respectively, once a firmware protocol exists to speak to.
 */
export interface Transport {
  readonly kind: TransportKind;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  send(command: OutgoingCommand): Promise<void>;
  onMessage(listener: MessageListener): () => void;
  isConnected(): boolean;
}
