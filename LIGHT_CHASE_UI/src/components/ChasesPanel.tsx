import { useStoreActions, useStoreState } from '../store';
import { ChaseEditor } from './ChaseEditor';

export function ChasesPanel() {
  const chases = useStoreState((state) => state.chases.items);
  const selectedId = useStoreState((state) => state.chases.selectedId);
  const select = useStoreActions((actions) => actions.chases.select);
  const addChase = useStoreActions((actions) => actions.chases.addChase);
  const removeChase = useStoreActions((actions) => actions.chases.removeChase);

  return (
    <div className="panel chases-panel">
      <aside className="card sidebar">
        <div className="sidebar-header">
          <h2>Chases</h2>
          <button className="btn-secondary" onClick={() => addChase(undefined)}>
            + New
          </button>
        </div>
        <ul className="item-list">
          {chases.map((chase) => (
            <li key={chase.id} className={chase.id === selectedId ? 'item-row item-row-active' : 'item-row'}>
              <button className="item-row-button" onClick={() => select(chase.id)}>
                {chase.name}
                <span className="item-row-meta">{chase.steps.length} steps</span>
              </button>
              <button className="btn-icon" title="Delete chase" onClick={() => removeChase(chase.id)}>
                ✕
              </button>
            </li>
          ))}
        </ul>
      </aside>
      <section className="editor-area">
        <ChaseEditor />
      </section>
    </div>
  );
}
