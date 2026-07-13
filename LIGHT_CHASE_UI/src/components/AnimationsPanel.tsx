import { useState } from 'react';
import { useStoreActions, useStoreState } from '../store';
import type { AnimationKind } from '../types';

const KIND_OPTIONS: Array<{ id: AnimationKind; label: string }> = [
  { id: 'chase', label: 'Chase (uses a saved chase)' },
  { id: 'strobe', label: 'Strobe' },
  { id: 'fadeAll', label: 'Fade All' },
  { id: 'wave', label: 'Wave' },
  { id: 'random', label: 'Random' },
];

export function AnimationsPanel() {
  const animations = useStoreState((state) => state.animations.items);
  const selectedId = useStoreState((state) => state.animations.selectedId);
  const selected = useStoreState((state) => state.animations.selected);
  const chases = useStoreState((state) => state.chases.items);
  const connected = useStoreState((state) => state.connection.status.connected);

  const select = useStoreActions((actions) => actions.animations.select);
  const addAnimation = useStoreActions((actions) => actions.animations.addAnimation);
  const removeAnimation = useStoreActions((actions) => actions.animations.removeAnimation);
  const renameAnimation = useStoreActions((actions) => actions.animations.renameAnimation);
  const updateParams = useStoreActions((actions) => actions.animations.updateParams);
  const setChaseRef = useStoreActions((actions) => actions.animations.setChaseRef);
  const sendCommand = useStoreActions((actions) => actions.connection.sendCommand);

  const [newName, setNewName] = useState('');
  const [newKind, setNewKind] = useState<AnimationKind>('strobe');

  return (
    <div className="panel chases-panel">
      <aside className="card sidebar">
        <div className="sidebar-header">
          <h2>Animations</h2>
        </div>
        <ul className="item-list">
          {animations.map((animation) => (
            <li
              key={animation.id}
              className={animation.id === selectedId ? 'item-row item-row-active' : 'item-row'}
            >
              <button className="item-row-button" onClick={() => select(animation.id)}>
                {animation.name}
                <span className="item-row-meta">{animation.kind}</span>
              </button>
              <button className="btn-icon" title="Delete animation" onClick={() => removeAnimation(animation.id)}>
                ✕
              </button>
            </li>
          ))}
        </ul>

        <form
          className="new-animation-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!newName.trim()) return;
            const chaseId = newKind === 'chase' ? chases[0]?.id : undefined;
            addAnimation({ name: newName, kind: newKind, chaseId });
            setNewName('');
          }}
        >
          <input
            placeholder="New animation name"
            value={newName}
            onChange={(event) => setNewName(event.target.value)}
          />
          <select value={newKind} onChange={(event) => setNewKind(event.target.value as AnimationKind)}>
            {KIND_OPTIONS.map((option) => (
              <option key={option.id} value={option.id}>
                {option.label}
              </option>
            ))}
          </select>
          <button className="btn-secondary" type="submit">
            + Add
          </button>
        </form>
      </aside>

      <section className="editor-area">
        {!selected ? (
          <div className="card empty-state">
            <p>No animation selected.</p>
          </div>
        ) : (
          <div className="card">
            <div className="editor-header">
              <input
                className="name-input"
                value={selected.name}
                onChange={(event) => renameAnimation({ id: selected.id, name: event.target.value })}
              />
              <span className="kind-badge">{selected.kind}</span>
              <button
                className="btn-primary"
                disabled={!connected}
                title={connected ? undefined : 'Connect to a device first'}
                onClick={() => sendCommand({ type: 'applyAnimation', animation: selected })}
              >
                Activate
              </button>
            </div>

            {selected.kind === 'chase' && (
              <label className="field-row">
                Chase
                <select
                  value={selected.chaseId ?? ''}
                  onChange={(event) => setChaseRef({ id: selected.id, chaseId: event.target.value })}
                >
                  {chases.map((chase) => (
                    <option key={chase.id} value={chase.id}>
                      {chase.name}
                    </option>
                  ))}
                </select>
              </label>
            )}

            <label className="field-row">
              Speed (ms)
              <input
                type="number"
                min={10}
                className="number-input"
                value={selected.params.speedMs}
                onChange={(event) =>
                  updateParams({ id: selected.id, patch: { speedMs: Number(event.target.value) } })
                }
              />
            </label>

            <label className="field-row">
              Intensity
              <input
                type="range"
                min={0}
                max={1}
                step={0.05}
                value={selected.params.intensity}
                onChange={(event) =>
                  updateParams({ id: selected.id, patch: { intensity: Number(event.target.value) } })
                }
              />
              <span>{Math.round(selected.params.intensity * 100)}%</span>
            </label>
          </div>
        )}
      </section>
    </div>
  );
}
