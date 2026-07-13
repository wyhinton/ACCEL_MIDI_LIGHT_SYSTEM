import { action, Action } from 'easy-peasy';

export type AppTab = 'connection' | 'chases' | 'animations' | 'layout';

export interface UiModel {
  activeTab: AppTab;
  setActiveTab: Action<UiModel, AppTab>;
}

export const uiModel: UiModel = {
  activeTab: 'connection',
  setActiveTab: action((state, tab) => {
    state.activeTab = tab;
  }),
};
