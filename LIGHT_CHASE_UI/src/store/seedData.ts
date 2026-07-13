import type { AnimationPreset, ChaseDefinition, DeviceChannel } from '../types';

/**
 * Default channels mirror the physical layout in the current firmware:
 * MULTIPLEX_8266 drives 3 local MOSFET channels directly, and commands
 * MOSFET_EXTENDER_8266 over the UART link for 2 more (see
 * MULTIPLEX_8266/src/main.cpp ChaseStep enum + sendLinkCommand()).
 */
export const DEFAULT_CHANNELS: DeviceChannel[] = [
  { id: 'local-a', label: 'Local A', physicalRef: 'MULTIPLEX GPIO0' },
  { id: 'local-b', label: 'Local B', physicalRef: 'MULTIPLEX GPIO4' },
  { id: 'local-c', label: 'Local C', physicalRef: 'MULTIPLEX GPIO5' },
  { id: 'ext-0', label: 'Extender 0', physicalRef: 'EXTENDER GPIO4 (link ch0)' },
  { id: 'ext-1', label: 'Extender 1', physicalRef: 'EXTENDER GPIO5 (link ch1)' },
];

function step(id: string, channelId: string): ChaseDefinition['steps'][number] {
  return {
    id,
    activeChannelIds: [channelId],
    durationMs: 500,
    fade: true,
    fadeDurationMs: 250,
  };
}

/** Reproduces the hardcoded CHASE_PIN_C -> CHASE_PIN_A -> CHASE_PIN_B -> CHASE_EXT_B round robin. */
export const DEFAULT_CHASES: ChaseDefinition[] = [
  {
    id: 'factory-round-robin',
    name: 'Factory Round Robin',
    loop: true,
    steps: [
      step('step-1', 'local-a'),
      step('step-2', 'local-b'),
      step('step-3', 'local-c'),
      step('step-4', 'ext-1'),
    ],
  },
];

export const DEFAULT_ANIMATIONS: AnimationPreset[] = [
  {
    id: 'classic-chase',
    name: 'Classic Chase',
    kind: 'chase',
    chaseId: 'factory-round-robin',
    params: { speedMs: 500, intensity: 1 },
  },
  {
    id: 'full-strobe',
    name: 'Full Strobe',
    kind: 'strobe',
    params: { speedMs: 100, intensity: 1 },
  },
  {
    id: 'fade-all',
    name: 'Fade All',
    kind: 'fadeAll',
    params: { speedMs: 1000, intensity: 0.8 },
  },
];
