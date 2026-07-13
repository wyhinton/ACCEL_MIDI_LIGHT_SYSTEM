import type { Transport } from '../transport';

/** Mutable holder for the active transport, injected into thunks so models never import the store directly. */
export interface StoreInjections {
  transportRef: { current: Transport | null };
}

export function createInjections(): StoreInjections {
  return { transportRef: { current: null } };
}
