import { useMemo, useState } from 'react';
import { usePlaybackClock } from '../playback/usePlaybackClock';
import type { PreviewSource } from '../playback/simulate';
import { computeIntensities } from '../playback/simulate';
import { useStoreActions, useStoreState } from '../store';
import { DEFAULT_CHANNELS } from '../store/seedData';
import { LightGrid } from './LightGrid';

type SourceKind = 'chase' | 'animation';

const ALL_CHANNEL_IDS = DEFAULT_CHANNELS.map((channel) => channel.id);

export function LayoutPanel() {
  const rows = useStoreState((state) => state.layout.rows);
  const cols = useStoreState((state) => state.layout.cols);
  const cells = useStoreState((state) => state.layout.cells);
  const setSize = useStoreActions((actions) => actions.layout.setSize);
  const assignChannel = useStoreActions((actions) => actions.layout.assignChannel);

  const chases = useStoreState((state) => state.chases.items);
  const animations = useStoreState((state) => state.animations.items);

  const [sourceKind, setSourceKind] = useState<SourceKind>('chase');
  const [sourceId, setSourceId] = useState<string | null>(chases[0]?.id ?? null);
  const [playing, setPlaying] = useState(false);
  const [elapsed, resetClock] = usePlaybackClock(playing);

  const source: PreviewSource | null = useMemo(() => {
    if (sourceKind === 'chase') {
      const chase = chases.find((item) => item.id === sourceId);
      return chase ? { kind: 'chase', chase } : null;
    }
    const animation = animations.find((item) => item.id === sourceId);
    if (!animation) return null;
    const resolvedChase =
      animation.kind === 'chase' ? chases.find((chase) => chase.id === animation.chaseId) : undefined;
    return { kind: 'animation', animation, resolvedChase };
  }, [sourceKind, sourceId, chases, animations]);

  const intensities = useMemo(
    () => computeIntensities(source, elapsed, ALL_CHANNEL_IDS),
    [source, elapsed],
  );

  const handleStop = () => {
    setPlaying(false);
    resetClock();
  };

  const options = sourceKind === 'chase' ? chases : animations;

  return (
    <div className="panel layout-panel">
      <aside className="card sidebar layout-sidebar">
        <h2>Grid Size</h2>
        <div className="size-controls">
          <label>
            Rows
            <input
              type="number"
              min={1}
              max={12}
              value={rows}
              onChange={(event) => setSize({ rows: Number(event.target.value), cols })}
            />
          </label>
          <label>
            Cols
            <input
              type="number"
              min={1}
              max={12}
              value={cols}
              onChange={(event) => setSize({ rows, cols: Number(event.target.value) })}
            />
          </label>
        </div>

        <h2>Preview</h2>
        <label className="field-row">
          Source
          <select
            value={sourceKind}
            onChange={(event) => {
              setSourceKind(event.target.value as SourceKind);
              setSourceId(null);
              handleStop();
            }}
          >
            <option value="chase">Chase</option>
            <option value="animation">Animation</option>
          </select>
        </label>
        <label className="field-row">
          {sourceKind === 'chase' ? 'Chase' : 'Animation'}
          <select
            value={sourceId ?? ''}
            onChange={(event) => {
              setSourceId(event.target.value);
              handleStop();
            }}
          >
            <option value="" disabled>
              Select…
            </option>
            {options.map((item) => (
              <option key={item.id} value={item.id}>
                {item.name}
              </option>
            ))}
          </select>
        </label>

        <div className="playback-controls">
          <button className="btn-primary" disabled={!source || playing} onClick={() => setPlaying(true)}>
            ▶ Play
          </button>
          <button className="btn-secondary" disabled={!playing} onClick={() => setPlaying(false)}>
            ⏸ Pause
          </button>
          <button className="btn-secondary" onClick={handleStop}>
            ⏹ Stop
          </button>
        </div>
      </aside>

      <section className="editor-area">
        <LightGrid
          rows={rows}
          cols={cols}
          cells={cells}
          channels={DEFAULT_CHANNELS}
          intensities={intensities}
          onAssign={(cellId, channelId) => assignChannel({ cellId, channelId })}
        />
      </section>
    </div>
  );
}
