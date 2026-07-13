import { AnimationsPanel } from './components/AnimationsPanel';
import { ChasesPanel } from './components/ChasesPanel';
import { ConnectionPanel } from './components/ConnectionPanel';
import { LayoutPanel } from './components/LayoutPanel';
import type { AppTab } from './store';
import { useStoreActions, useStoreState } from './store';

const TABS: Array<{ id: AppTab; label: string }> = [
  { id: 'connection', label: 'Connection' },
  { id: 'chases', label: 'Chases' },
  { id: 'animations', label: 'Animations' },
  { id: 'layout', label: 'Layout' },
];

export default function App() {
  const activeTab = useStoreState((state) => state.ui.activeTab);
  const setActiveTab = useStoreActions((actions) => actions.ui.setActiveTab);
  const status = useStoreState((state) => state.connection.status);

  return (
    <div className="app">
      <header className="app-header">
        <h1>Light Chase Configurator</h1>
        <div className="header-status">
          <span className={status.connected ? 'status-dot status-dot-on' : 'status-dot'} />
          {status.connected ? `Connected (${status.transportKind})` : 'Disconnected'}
        </div>
      </header>

      <nav className="tabs">
        {TABS.map((tab) => (
          <button
            key={tab.id}
            className={tab.id === activeTab ? 'tab tab-active' : 'tab'}
            onClick={() => setActiveTab(tab.id)}
          >
            {tab.label}
          </button>
        ))}
      </nav>

      <main className="app-main">
        {activeTab === 'connection' && <ConnectionPanel />}
        {activeTab === 'chases' && <ChasesPanel />}
        {activeTab === 'animations' && <AnimationsPanel />}
        {activeTab === 'layout' && <LayoutPanel />}
      </main>
    </div>
  );
}
