import { createStore, createTypedHooks } from 'easy-peasy';
import { animationModel, AnimationModel } from './animationModel';
import { chaseModel, ChaseModel } from './chaseModel';
import { connectionModel, ConnectionModel } from './connectionModel';
import { createInjections } from './injections';
import { layoutModel, LayoutModel } from './layoutModel';
import { uiModel, UiModel } from './uiModel';

export interface StoreModel {
  connection: ConnectionModel;
  chases: ChaseModel;
  animations: AnimationModel;
  layout: LayoutModel;
  ui: UiModel;
}

export const store = createStore<StoreModel>(
  {
    connection: connectionModel,
    chases: chaseModel,
    animations: animationModel,
    layout: layoutModel,
    ui: uiModel,
  },
  { injections: createInjections() },
);

const typedHooks = createTypedHooks<StoreModel>();
export const useStoreActions = typedHooks.useStoreActions;
export const useStoreState = typedHooks.useStoreState;
export const useStoreDispatch = typedHooks.useStoreDispatch;

export type { AppTab } from './uiModel';
export type { AnimationModel } from './animationModel';
export type { ChaseModel } from './chaseModel';
export type { ConnectionModel } from './connectionModel';
export type { LayoutModel } from './layoutModel';
