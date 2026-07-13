import { useStoreActions, useStoreState } from '../store';
import { ChannelChips } from './ChannelChips';

export function ChaseEditor() {
  const chase = useStoreState((state) => state.chases.selected);
  const connected = useStoreState((state) => state.connection.status.connected);
  const renameChase = useStoreActions((actions) => actions.chases.renameChase);
  const setLoop = useStoreActions((actions) => actions.chases.setLoop);
  const addStep = useStoreActions((actions) => actions.chases.addStep);
  const removeStep = useStoreActions((actions) => actions.chases.removeStep);
  const updateStep = useStoreActions((actions) => actions.chases.updateStep);
  const moveStep = useStoreActions((actions) => actions.chases.moveStep);
  const toggleStepChannel = useStoreActions((actions) => actions.chases.toggleStepChannel);
  const sendCommand = useStoreActions((actions) => actions.connection.sendCommand);

  if (!chase) {
    return (
      <div className="card empty-state">
        <p>No chase selected. Create one to get started.</p>
      </div>
    );
  }

  return (
    <div className="card">
      <div className="editor-header">
        <input
          className="name-input"
          value={chase.name}
          onChange={(event) => renameChase({ id: chase.id, name: event.target.value })}
        />
        <label className="loop-toggle">
          <input
            type="checkbox"
            checked={chase.loop}
            onChange={(event) => setLoop({ id: chase.id, loop: event.target.checked })}
          />
          Loop
        </label>
        <button
          className="btn-primary"
          disabled={!connected}
          title={connected ? undefined : 'Connect to a device first'}
          onClick={() => sendCommand({ type: 'applyChase', chase })}
        >
          Send to Device
        </button>
      </div>

      <table className="step-table">
        <thead>
          <tr>
            <th>#</th>
            <th>Channels</th>
            <th>Duration (ms)</th>
            <th>Fade</th>
            <th>Fade (ms)</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {chase.steps.map((step, index) => (
            <tr key={step.id}>
              <td>{index + 1}</td>
              <td>
                <ChannelChips
                  activeChannelIds={step.activeChannelIds}
                  onToggle={(channelId) => toggleStepChannel({ chaseId: chase.id, stepId: step.id, channelId })}
                />
              </td>
              <td>
                <input
                  type="number"
                  min={1}
                  className="number-input"
                  value={step.durationMs}
                  onChange={(event) =>
                    updateStep({
                      chaseId: chase.id,
                      stepId: step.id,
                      patch: { durationMs: Number(event.target.value) },
                    })
                  }
                />
              </td>
              <td>
                <input
                  type="checkbox"
                  checked={step.fade}
                  onChange={(event) =>
                    updateStep({ chaseId: chase.id, stepId: step.id, patch: { fade: event.target.checked } })
                  }
                />
              </td>
              <td>
                <input
                  type="number"
                  min={1}
                  className="number-input"
                  disabled={!step.fade}
                  value={step.fadeDurationMs}
                  onChange={(event) =>
                    updateStep({
                      chaseId: chase.id,
                      stepId: step.id,
                      patch: { fadeDurationMs: Number(event.target.value) },
                    })
                  }
                />
              </td>
              <td className="step-actions">
                <button
                  className="btn-icon"
                  disabled={index === 0}
                  title="Move up"
                  onClick={() => moveStep({ chaseId: chase.id, stepId: step.id, direction: 'up' })}
                >
                  ↑
                </button>
                <button
                  className="btn-icon"
                  disabled={index === chase.steps.length - 1}
                  title="Move down"
                  onClick={() => moveStep({ chaseId: chase.id, stepId: step.id, direction: 'down' })}
                >
                  ↓
                </button>
                <button
                  className="btn-icon"
                  title="Delete step"
                  onClick={() => removeStep({ chaseId: chase.id, stepId: step.id })}
                >
                  ✕
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>

      <button className="btn-secondary" onClick={() => addStep({ chaseId: chase.id })}>
        + Add Step
      </button>
    </div>
  );
}
