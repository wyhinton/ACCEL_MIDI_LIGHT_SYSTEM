import { useStoreActions, useStoreState } from '../store';
import type { TransportKind } from '../types';

const TRANSPORT_OPTIONS: Array<{ id: TransportKind; label: string; description: string }> = [
  {
    id: 'mock',
    label: 'Mock Simulator',
    description: 'No hardware required — simulates a device for UI development.',
  },
  {
    id: 'wired',
    label: 'Wired (USB)',
    description: 'Not implemented yet — falls back to the mock simulator.',
  },
  {
    id: 'wireless',
    label: 'Wireless (WiFi)',
    description: 'Not implemented yet — falls back to the mock simulator.',
  },
];

export function ConnectionPanel() {
  const status = useStoreState((state) => state.connection.status);
  const log = useStoreState((state) => state.connection.log);
  const connect = useStoreActions((actions) => actions.connection.connect);
  const disconnect = useStoreActions((actions) => actions.connection.disconnect);
  const clearLog = useStoreActions((actions) => actions.connection.clearLog);

  return (
    <div className="panel connection-panel">
      <section className="card">
        <h2>Device Connection</h2>
        <div className="transport-options">
          {TRANSPORT_OPTIONS.map((option) => {
            const isActive = status.transportKind === option.id;
            const disableOthers = status.connected && !isActive;
            return (
              <button
                key={option.id}
                className={isActive ? 'transport-option transport-option-active' : 'transport-option'}
                disabled={status.connecting || disableOthers}
                onClick={() => connect(option.id)}
              >
                <strong>{option.label}</strong>
                <span>{option.description}</span>
              </button>
            );
          })}
        </div>

        <div className="status-row">
          <span className={status.connected ? 'status-dot status-dot-on' : 'status-dot'} />
          <span>
            {status.connecting
              ? 'Connecting…'
              : status.connected
                ? `Connected via ${status.transportKind}${status.firmwareVersion ? ` — firmware ${status.firmwareVersion}` : ''}`
                : 'Disconnected'}
          </span>
          {status.connected && (
            <button className="btn-secondary" onClick={() => disconnect()}>
              Disconnect
            </button>
          )}
        </div>
        {status.lastError && <p className="error-text">{status.lastError}</p>}
      </section>

      <section className="card log-card">
        <div className="log-header">
          <h2>Activity Log</h2>
          <button className="btn-secondary" onClick={() => clearLog()}>
            Clear
          </button>
        </div>
        <div className="log-lines">
          {log.length === 0 ? (
            <p className="log-empty">No activity yet.</p>
          ) : (
            log.map((line, index) => <div key={index}>{line}</div>)
          )}
        </div>
      </section>
    </div>
  );
}
