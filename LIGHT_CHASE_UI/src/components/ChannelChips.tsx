import { DEFAULT_CHANNELS } from '../store/seedData';

interface ChannelChipsProps {
  activeChannelIds: string[];
  onToggle: (channelId: string) => void;
}

export function ChannelChips({ activeChannelIds, onToggle }: ChannelChipsProps) {
  return (
    <div className="channel-chips">
      {DEFAULT_CHANNELS.map((channel) => {
        const active = activeChannelIds.includes(channel.id);
        return (
          <button
            key={channel.id}
            type="button"
            className={active ? 'channel-chip channel-chip-active' : 'channel-chip'}
            title={channel.physicalRef}
            onClick={() => onToggle(channel.id)}
          >
            {channel.label}
          </button>
        );
      })}
    </div>
  );
}
