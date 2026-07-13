import { action, Action, computed, Computed } from 'easy-peasy';
import type { ChaseDefinition, ChaseStep } from '../types';
import { DEFAULT_CHASES } from './seedData';

function makeId(prefix: string): string {
  return `${prefix}-${crypto.randomUUID()}`;
}

function makeStep(): ChaseStep {
  return {
    id: makeId('step'),
    activeChannelIds: [],
    durationMs: 500,
    fade: false,
    fadeDurationMs: 250,
  };
}

export interface ChaseModel {
  items: ChaseDefinition[];
  selectedId: string | null;

  selected: Computed<ChaseModel, ChaseDefinition | undefined>;

  select: Action<ChaseModel, string>;
  addChase: Action<ChaseModel, { name: string } | undefined>;
  removeChase: Action<ChaseModel, string>;
  renameChase: Action<ChaseModel, { id: string; name: string }>;
  setLoop: Action<ChaseModel, { id: string; loop: boolean }>;

  addStep: Action<ChaseModel, { chaseId: string }>;
  removeStep: Action<ChaseModel, { chaseId: string; stepId: string }>;
  updateStep: Action<ChaseModel, { chaseId: string; stepId: string; patch: Partial<ChaseStep> }>;
  moveStep: Action<ChaseModel, { chaseId: string; stepId: string; direction: 'up' | 'down' }>;
  toggleStepChannel: Action<ChaseModel, { chaseId: string; stepId: string; channelId: string }>;
}

export const chaseModel: ChaseModel = {
  items: DEFAULT_CHASES,
  selectedId: DEFAULT_CHASES[0]?.id ?? null,

  selected: computed((state) => state.items.find((chase) => chase.id === state.selectedId)),

  select: action((state, id) => {
    state.selectedId = id;
  }),

  addChase: action((state, payload) => {
    const chase: ChaseDefinition = {
      id: makeId('chase'),
      name: payload?.name?.trim() || `New Chase ${state.items.length + 1}`,
      loop: true,
      steps: [makeStep()],
    };
    state.items.push(chase);
    state.selectedId = chase.id;
  }),

  removeChase: action((state, id) => {
    state.items = state.items.filter((chase) => chase.id !== id);
    if (state.selectedId === id) {
      state.selectedId = state.items[0]?.id ?? null;
    }
  }),

  renameChase: action((state, { id, name }) => {
    const chase = state.items.find((item) => item.id === id);
    if (chase) chase.name = name;
  }),

  setLoop: action((state, { id, loop }) => {
    const chase = state.items.find((item) => item.id === id);
    if (chase) chase.loop = loop;
  }),

  addStep: action((state, { chaseId }) => {
    const chase = state.items.find((item) => item.id === chaseId);
    chase?.steps.push(makeStep());
  }),

  removeStep: action((state, { chaseId, stepId }) => {
    const chase = state.items.find((item) => item.id === chaseId);
    if (chase) chase.steps = chase.steps.filter((s) => s.id !== stepId);
  }),

  updateStep: action((state, { chaseId, stepId, patch }) => {
    const chase = state.items.find((item) => item.id === chaseId);
    const step = chase?.steps.find((s) => s.id === stepId);
    if (step) Object.assign(step, patch);
  }),

  moveStep: action((state, { chaseId, stepId, direction }) => {
    const chase = state.items.find((item) => item.id === chaseId);
    if (!chase) return;
    const index = chase.steps.findIndex((s) => s.id === stepId);
    const targetIndex = direction === 'up' ? index - 1 : index + 1;
    if (index < 0 || targetIndex < 0 || targetIndex >= chase.steps.length) return;
    const [removed] = chase.steps.splice(index, 1);
    chase.steps.splice(targetIndex, 0, removed);
  }),

  toggleStepChannel: action((state, { chaseId, stepId, channelId }) => {
    const chase = state.items.find((item) => item.id === chaseId);
    const step = chase?.steps.find((s) => s.id === stepId);
    if (!step) return;
    const index = step.activeChannelIds.indexOf(channelId);
    if (index >= 0) {
      step.activeChannelIds.splice(index, 1);
    } else {
      step.activeChannelIds.push(channelId);
    }
  }),
};
