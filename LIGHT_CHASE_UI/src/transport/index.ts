import type { TransportKind } from '../types';
import { MockTransport } from './MockTransport';
import type { Transport } from './Transport';

export type { MessageListener, Transport } from './Transport';
export { MockTransport } from './MockTransport';

/**
 * Factory for the transport the connection store should use.
 *
 * 'wired' and 'wireless' fall back to the mock for now — real Web Serial and
 * WebSocket/TCP implementations need a firmware-side protocol to target first
 * (see MULTIPLEX_8266/src/main.cpp, which today only has single-keystroke
 * USB serial commands and no WiFi at all). Swap the cases below once that
 * protocol exists; nothing else in the app needs to change since callers only
 * depend on the Transport interface.
 */
export function createTransport(kind: TransportKind): Transport {
  switch (kind) {
    case 'mock':
      return new MockTransport();
    case 'wired':
    case 'wireless':
      return new MockTransport();
    default: {
      const exhaustiveCheck: never = kind;
      throw new Error(`Unknown transport kind: ${exhaustiveCheck}`);
    }
  }
}
