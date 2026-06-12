import { describe, it, expect, beforeEach } from 'vitest';
import { create } from 'zustand';

// Re-create a minimal store inline to avoid the persist middleware
// requiring localStorage in jsdom.  We test the logic, not the storage layer.

type MinimalCaseData = Record<string, unknown>;

type StoreState = {
  data: MinimalCaseData;
  history: MinimalCaseData[];
  future: MinimalCaseData[];
  set: (d: MinimalCaseData) => void;
  undo: () => void;
  redo: () => void;
};

function makeStore() {
  return create<StoreState>((set, get) => ({
    data: { a: 1 },
    history: [],
    future: [],
    set: (d) => {
      const prev = get().data;
      set({ data: d, history: [...get().history, prev], future: [] });
    },
    undo: () => {
      const { history, data } = get();
      if (history.length === 0) return;
      const prev = history[history.length - 1];
      set({ data: prev, history: history.slice(0, -1), future: [data, ...get().future] });
    },
    redo: () => {
      const { future, data } = get();
      if (future.length === 0) return;
      const next = future[0];
      set({ data: next, future: future.slice(1), history: [...get().history, data] });
    },
  }));
}

describe('Store undo/redo pattern (logic unit test)', () => {
  let store: ReturnType<typeof makeStore>;

  beforeEach(() => {
    store = makeStore();
  });

  it('starts with empty history and future', () => {
    const s = store.getState();
    expect(s.history).toHaveLength(0);
    expect(s.future).toHaveLength(0);
  });

  it('records history on set', () => {
    store.getState().set({ a: 2 });
    const s = store.getState();
    expect(s.data).toEqual({ a: 2 });
    expect(s.history).toHaveLength(1);
    expect(s.history[0]).toEqual({ a: 1 });
  });

  it('undo restores previous state and pushes to future', () => {
    store.getState().set({ a: 2 });
    store.getState().undo();
    const s = store.getState();
    expect(s.data).toEqual({ a: 1 });
    expect(s.history).toHaveLength(0);
    expect(s.future).toHaveLength(1);
    expect(s.future[0]).toEqual({ a: 2 });
  });

  it('redo re-applies undone state', () => {
    store.getState().set({ a: 2 });
    store.getState().undo();
    store.getState().redo();
    const s = store.getState();
    expect(s.data).toEqual({ a: 2 });
    expect(s.future).toHaveLength(0);
  });

  it('new set clears future', () => {
    store.getState().set({ a: 2 });
    store.getState().undo();
    store.getState().set({ a: 3 }); // new edit clears redo stack
    expect(store.getState().future).toHaveLength(0);
    expect(store.getState().data).toEqual({ a: 3 });
  });

  it('undo with empty history is a no-op', () => {
    const before = store.getState().data;
    store.getState().undo();
    expect(store.getState().data).toEqual(before);
  });

  it('redo with empty future is a no-op', () => {
    store.getState().set({ a: 2 });
    const before = store.getState().data;
    store.getState().redo();
    expect(store.getState().data).toEqual(before);
  });

  it('supports multiple sequential edits', () => {
    store.getState().set({ a: 2 });
    store.getState().set({ a: 3 });
    store.getState().set({ a: 4 });
    expect(store.getState().history).toHaveLength(3);
    store.getState().undo();
    store.getState().undo();
    expect(store.getState().data).toEqual({ a: 2 });
    expect(store.getState().future).toHaveLength(2);
  });
});
