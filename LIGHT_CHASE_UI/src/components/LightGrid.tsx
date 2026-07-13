import type { ChannelIntensities } from '../playback/simulate';
import type { DeviceChannel, LayoutCell } from '../types';

interface LightGridProps {
  rows: number;
  cols: number;
  cells: LayoutCell[];
  channels: DeviceChannel[];
  intensities: ChannelIntensities;
  onAssign: (cellId: string, channelId: string | null) => void;
}

export function LightGrid({ rows, cols, cells, channels, intensities, onAssign }: LightGridProps) {
  return (
    <div
      className="light-grid"
      style={{ gridTemplateColumns: `repeat(${cols}, 1fr)`, gridTemplateRows: `repeat(${rows}, 1fr)` }}
    >
      {cells.map((cell) => {
        const channel = channels.find((c) => c.id === cell.channelId);
        const intensity = channel ? (intensities[channel.id] ?? 0) : 0;
        return (
          <div key={cell.id} className="grid-cell">
            <div
              className={channel ? 'grid-light grid-light-assigned' : 'grid-light'}
              title={channel?.physicalRef}
              style={{
                opacity: channel ? 0.18 + intensity * 0.82 : 1,
                boxShadow: channel ? `0 0 ${8 + intensity * 28}px rgba(255, 196, 92, ${intensity})` : 'none',
              }}
            />
            <select
              className="grid-cell-select"
              value={cell.channelId ?? ''}
              onChange={(event) => onAssign(cell.id, event.target.value || null)}
            >
              <option value="">—</option>
              {channels.map((c) => (
                <option key={c.id} value={c.id}>
                  {c.label}
                </option>
              ))}
            </select>
          </div>
        );
      })}
    </div>
  );
}
