import { action, Action } from 'easy-peasy';
import type { LayoutCell } from '../types';
import { DEFAULT_CHANNELS } from './seedData';

const DEFAULT_ROWS = 3;
const DEFAULT_COLS = 4;
const MIN_SIZE = 1;
const MAX_SIZE = 12;

/** Seeds the grid with the default channels in row-major order so the preview isn't empty on first load. */
function initialCells(rows: number, cols: number): LayoutCell[] {
  const seedChannelIds = DEFAULT_CHANNELS.map((channel) => channel.id);
  const cells: LayoutCell[] = [];
  let seedIndex = 0;
  for (let row = 0; row < rows; row++) {
    for (let col = 0; col < cols; col++) {
      cells.push({
        id: `${row}-${col}`,
        row,
        col,
        channelId: seedIndex < seedChannelIds.length ? seedChannelIds[seedIndex] : null,
      });
      seedIndex++;
    }
  }
  return cells;
}

/** Rebuilds the cell list for a new size, carrying forward assignments for cells that still exist. */
function resizeCells(rows: number, cols: number, existing: LayoutCell[]): LayoutCell[] {
  const lookup = new Map(existing.map((cell) => [cell.id, cell.channelId]));
  const cells: LayoutCell[] = [];
  for (let row = 0; row < rows; row++) {
    for (let col = 0; col < cols; col++) {
      const id = `${row}-${col}`;
      cells.push({ id, row, col, channelId: lookup.get(id) ?? null });
    }
  }
  return cells;
}

export interface LayoutModel {
  rows: number;
  cols: number;
  cells: LayoutCell[];

  setSize: Action<LayoutModel, { rows: number; cols: number }>;
  assignChannel: Action<LayoutModel, { cellId: string; channelId: string | null }>;
}

export const layoutModel: LayoutModel = {
  rows: DEFAULT_ROWS,
  cols: DEFAULT_COLS,
  cells: initialCells(DEFAULT_ROWS, DEFAULT_COLS),

  setSize: action((state, { rows, cols }) => {
    const clampedRows = Math.min(Math.max(rows, MIN_SIZE), MAX_SIZE);
    const clampedCols = Math.min(Math.max(cols, MIN_SIZE), MAX_SIZE);
    state.rows = clampedRows;
    state.cols = clampedCols;
    state.cells = resizeCells(clampedRows, clampedCols, state.cells);
  }),

  assignChannel: action((state, { cellId, channelId }) => {
    const cell = state.cells.find((c) => c.id === cellId);
    if (cell) cell.channelId = channelId;
  }),
};
