import { action, Action, Actions, thunk, Thunk } from 'easy-peasy';
import { createTransport } from '../transport';
import type { DeviceStatus, IncomingMessage, OutgoingCommand, TransportKind } from '../types';
import type { StoreInjections } from './injections';

const MAX_LOG_LINES = 200;

export interface ConnectionModel {
  status: DeviceStatus;
  log: string[];

  setStatus: Action<ConnectionModel, Partial<DeviceStatus>>;
  pushLog: Action<ConnectionModel, string>;
  clearLog: Action<ConnectionModel>;

  connect: Thunk<ConnectionModel, TransportKind, StoreInjections>;
  disconnect: Thunk<ConnectionModel, void, StoreInjections>;
  sendCommand: Thunk<ConnectionModel, OutgoingCommand, StoreInjections>;
}

export const connectionModel: ConnectionModel = {
  status: {
    connected: false,
    connecting: false,
    transportKind: null,
  },
  log: [],

  setStatus: action((state, patch) => {
    state.status = { ...state.status, ...patch };
  }),

  pushLog: action((state, entry) => {
    const timestamp = new Date().toLocaleTimeString();
    state.log = [...state.log.slice(-(MAX_LOG_LINES - 1)), `${timestamp}  ${entry}`];
  }),

  clearLog: action((state) => {
    state.log = [];
  }),

  connect: thunk(async (actions, transportKind, { injections }) => {
    actions.setStatus({ connecting: true, lastError: undefined });
    try {
      await injections.transportRef.current?.disconnect();

      const transport = createTransport(transportKind);
      injections.transportRef.current = transport;
      transport.onMessage((message) => handleIncoming(actions, message));

      await transport.connect();
      actions.setStatus({ connected: true, connecting: false, transportKind });
      actions.pushLog(`Connected (${transportKind})`);
    } catch (error) {
      const messageText = error instanceof Error ? error.message : String(error);
      actions.setStatus({ connecting: false, lastError: messageText });
      actions.pushLog(`Connection failed: ${messageText}`);
    }
  }),

  disconnect: thunk(async (actions, _payload, { injections }) => {
    await injections.transportRef.current?.disconnect();
    injections.transportRef.current = null;
    actions.setStatus({ connected: false, connecting: false, transportKind: null });
    actions.pushLog('Disconnected');
  }),

  sendCommand: thunk(async (actions, command, { injections }) => {
    const transport = injections.transportRef.current;
    if (!transport || !transport.isConnected()) {
      actions.pushLog('Cannot send: not connected');
      return;
    }
    try {
      await transport.send(command);
    } catch (error) {
      const messageText = error instanceof Error ? error.message : String(error);
      actions.pushLog(`Send failed: ${messageText}`);
    }
  }),
};

function handleIncoming(actions: Actions<ConnectionModel>, message: IncomingMessage): void {
  switch (message.type) {
    case 'ready':
      actions.setStatus({ firmwareVersion: message.firmwareVersion });
      actions.pushLog(`Device ready: firmware ${message.firmwareVersion}`);
      break;
    case 'status':
      actions.setStatus({ linkOk: message.linkOk });
      break;
    case 'log':
      actions.pushLog(message.text);
      break;
    case 'ack':
      actions.pushLog(`Ack: ${message.command}`);
      break;
    case 'pong':
      actions.pushLog('Pong');
      break;
  }
}
