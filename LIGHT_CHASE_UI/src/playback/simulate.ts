import type { AnimationPreset, ChaseDefinition } from '../types';

/** What the grid preview is currently playing back. */
export type PreviewSource =
  | { kind: 'chase'; chase: ChaseDefinition }
  | { kind: 'animation'; animation: AnimationPreset; resolvedChase?: ChaseDefinition };

/** Per-channel brightness in [0, 1], keyed by channel id. Missing keys mean "off". */
export type ChannelIntensities = Record<string, number>;

export function computeIntensities(
  source: PreviewSource | null,
  elapsedMs: number,
  channelIds: string[],
): ChannelIntensities {
  if (!source) return {};

  if (source.kind === 'chase') {
    return computeChaseIntensities(source.chase, elapsedMs);
  }

  const { animation, resolvedChase } = source;
  switch (animation.kind) {
    case 'chase':
      return resolvedChase ? computeChaseIntensities(resolvedChase, elapsedMs) : {};
    case 'strobe':
      return computeStrobe(animation, elapsedMs, channelIds);
    case 'fadeAll':
      return computeFadeAll(animation, elapsedMs, channelIds);
    case 'wave':
      return computeWave(animation, elapsedMs, channelIds);
    case 'random':
      return computeRandom(animation, elapsedMs, channelIds);
  }
}

function computeChaseIntensities(chase: ChaseDefinition, elapsedMs: number): ChannelIntensities {
  const { steps } = chase;
  if (steps.length === 0) return {};

  const totalDuration = steps.reduce((sum, step) => sum + Math.max(step.durationMs, 1), 0);
  const t = chase.loop ? elapsedMs % totalDuration : Math.min(elapsedMs, totalDuration - 1);

  let cursor = 0;
  let activeIndex = steps.length - 1;
  let timeIntoStep = 0;
  for (let i = 0; i < steps.length; i++) {
    const duration = Math.max(steps[i].durationMs, 1);
    if (t < cursor + duration) {
      activeIndex = i;
      timeIntoStep = t - cursor;
      break;
    }
    cursor += duration;
  }

  const step = steps[activeIndex];
  const fadeProgress = step.fade ? Math.min(timeIntoStep / Math.max(step.fadeDurationMs, 1), 1) : 1;

  const result: ChannelIntensities = {};
  for (const channelId of step.activeChannelIds) {
    result[channelId] = fadeProgress;
  }
  return result;
}

function computeStrobe(animation: AnimationPreset, elapsedMs: number, channelIds: string[]): ChannelIntensities {
  const speed = Math.max(animation.params.speedMs, 1);
  const isOn = Math.floor(elapsedMs / speed) % 2 === 0;
  const level = isOn ? animation.params.intensity : 0;
  return Object.fromEntries(channelIds.map((id) => [id, level]));
}

function computeFadeAll(animation: AnimationPreset, elapsedMs: number, channelIds: string[]): ChannelIntensities {
  const period = Math.max(animation.params.speedMs, 1) * 2;
  const phase = (elapsedMs % period) / period;
  const level = ((Math.sin(phase * Math.PI * 2 - Math.PI / 2) + 1) / 2) * animation.params.intensity;
  return Object.fromEntries(channelIds.map((id) => [id, level]));
}

function computeWave(animation: AnimationPreset, elapsedMs: number, channelIds: string[]): ChannelIntensities {
  if (channelIds.length === 0) return {};
  const speed = Math.max(animation.params.speedMs, 1);
  const total = channelIds.length;
  const position = (elapsedMs / speed) % total;

  const result: ChannelIntensities = {};
  channelIds.forEach((id, index) => {
    const rawDistance = Math.abs(index - position);
    const distance = Math.min(rawDistance, total - rawDistance);
    result[id] = Math.max(0, 1 - distance) * animation.params.intensity;
  });
  return result;
}

function computeRandom(animation: AnimationPreset, elapsedMs: number, channelIds: string[]): ChannelIntensities {
  if (channelIds.length === 0) return {};
  const speed = Math.max(animation.params.speedMs, 1);
  const bucket = Math.floor(elapsedMs / speed);
  const activeIndex = pseudoRandomIndex(bucket, channelIds.length);

  const result: ChannelIntensities = {};
  channelIds.forEach((id, index) => {
    result[id] = index === activeIndex ? animation.params.intensity : 0;
  });
  return result;
}

/** Deterministic per-bucket pseudo-random index, so re-renders within the same bucket don't flicker. */
function pseudoRandomIndex(bucket: number, mod: number): number {
  if (mod <= 0) return 0;
  const hashed = Math.abs(Math.sin(bucket * 12.9898) * 43758.5453) % 1;
  return Math.floor(hashed * mod);
}
