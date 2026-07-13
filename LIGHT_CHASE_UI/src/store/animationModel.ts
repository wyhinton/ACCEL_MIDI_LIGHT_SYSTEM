import { action, Action, computed, Computed } from 'easy-peasy';
import type { AnimationKind, AnimationPreset } from '../types';
import { DEFAULT_ANIMATIONS } from './seedData';

function makeId(): string {
  return `animation-${crypto.randomUUID()}`;
}

export interface AnimationModel {
  items: AnimationPreset[];
  selectedId: string | null;

  selected: Computed<AnimationModel, AnimationPreset | undefined>;

  select: Action<AnimationModel, string>;
  addAnimation: Action<AnimationModel, { name: string; kind: AnimationKind; chaseId?: string }>;
  removeAnimation: Action<AnimationModel, string>;
  renameAnimation: Action<AnimationModel, { id: string; name: string }>;
  updateParams: Action<AnimationModel, { id: string; patch: Partial<AnimationPreset['params']> }>;
  setChaseRef: Action<AnimationModel, { id: string; chaseId: string }>;
}

export const animationModel: AnimationModel = {
  items: DEFAULT_ANIMATIONS,
  selectedId: DEFAULT_ANIMATIONS[0]?.id ?? null,

  selected: computed((state) => state.items.find((animation) => animation.id === state.selectedId)),

  select: action((state, id) => {
    state.selectedId = id;
  }),

  addAnimation: action((state, { name, kind, chaseId }) => {
    const animation: AnimationPreset = {
      id: makeId(),
      name: name.trim() || `New Animation ${state.items.length + 1}`,
      kind,
      chaseId: kind === 'chase' ? chaseId : undefined,
      params: { speedMs: 500, intensity: 1 },
    };
    state.items.push(animation);
    state.selectedId = animation.id;
  }),

  removeAnimation: action((state, id) => {
    state.items = state.items.filter((animation) => animation.id !== id);
    if (state.selectedId === id) {
      state.selectedId = state.items[0]?.id ?? null;
    }
  }),

  renameAnimation: action((state, { id, name }) => {
    const animation = state.items.find((item) => item.id === id);
    if (animation) animation.name = name;
  }),

  updateParams: action((state, { id, patch }) => {
    const animation = state.items.find((item) => item.id === id);
    if (animation) animation.params = { ...animation.params, ...patch };
  }),

  setChaseRef: action((state, { id, chaseId }) => {
    const animation = state.items.find((item) => item.id === id);
    if (animation) animation.chaseId = chaseId;
  }),
};
