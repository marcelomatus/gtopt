import { create } from 'zustand';
import { persist } from 'zustand/middleware';
import type { CaseData } from './api';
import type { AiConfig, AiMessage } from './ai-config';
import { DEFAULT_AI_CONFIG } from './ai-config';

const BLANK_CASE: CaseData = {
  case_name: 'new_case',
  options: {
    use_single_bus: true,
    scale_objective: 1000,
    demand_fail_cost: 1000,
    input_format: 'csv',
    output_format: 'csv',
  },
  simulation: {
    block_array: [{ uid: 1, duration: 1 }],
    stage_array: [{ uid: 1, first_block: 0, count_block: 1, active: 1 }],
    scenario_array: [{ uid: 1, probability_factor: 1 }],
  },
  system: {
    bus: [{ uid: 1, name: 'b1' }],
  },
  data_files: {},
};

export type Theme = 'light' | 'dark' | 'system';

type State = {
  caseData: CaseData;
  history: CaseData[];
  future: CaseData[];

  selectedScenario: number | null;
  selectedStage: number | null;
  selectedBlock: number | null;

  theme: Theme;

  results: Record<string, unknown> | null;
  aiConfig: AiConfig;
  aiMessages: AiMessage[];

  setCaseData: (data: CaseData) => void;
  updateCaseData: (mutator: (draft: CaseData) => CaseData) => void;
  resetCaseData: () => void;
  undo: () => void;
  redo: () => void;

  setSelection: (sel: {
    scenario?: number | null;
    stage?: number | null;
    block?: number | null;
  }) => void;

  setTheme: (theme: Theme) => void;

  setResults: (r: Record<string, unknown> | null) => void;
  setAiConfig: (config: Partial<AiConfig>) => void;
  addAiMessage: (msg: AiMessage) => void;
  updateAiMessage: (id: string, patch: Partial<AiMessage>) => void;
  clearAiMessages: () => void;
};

const MAX_HISTORY = 50;

export const useStore = create<State>()(
  persist(
    (set) => ({
      caseData: BLANK_CASE,
      history: [],
      future: [],
      selectedScenario: null,
      selectedStage: null,
      selectedBlock: null,
      theme: 'system',
      results: null,
      aiConfig: DEFAULT_AI_CONFIG,
      aiMessages: [],

      setCaseData: (data) =>
        set((state) => ({
          caseData: data,
          history: [state.caseData, ...state.history].slice(0, MAX_HISTORY),
          future: [],
        })),

      updateCaseData: (mutator) =>
        set((state) => ({
          caseData: mutator(state.caseData),
          history: [state.caseData, ...state.history].slice(0, MAX_HISTORY),
          future: [],
        })),

      resetCaseData: () =>
        set((state) => ({
          caseData: BLANK_CASE,
          history: [state.caseData, ...state.history].slice(0, MAX_HISTORY),
          future: [],
        })),

      undo: () =>
        set((state) => {
          if (state.history.length === 0) return state;
          const [prev, ...rest] = state.history;
          return {
            caseData: prev,
            history: rest,
            future: [state.caseData, ...state.future].slice(0, MAX_HISTORY),
          };
        }),

      redo: () =>
        set((state) => {
          if (state.future.length === 0) return state;
          const [next, ...rest] = state.future;
          return {
            caseData: next,
            future: rest,
            history: [state.caseData, ...state.history].slice(0, MAX_HISTORY),
          };
        }),

      setSelection: (sel) =>
        set((state) => ({
          selectedScenario:
            sel.scenario === undefined ? state.selectedScenario : sel.scenario,
          selectedStage:
            sel.stage === undefined ? state.selectedStage : sel.stage,
          selectedBlock:
            sel.block === undefined ? state.selectedBlock : sel.block,
        })),

      setTheme: (theme) => set({ theme }),

      setResults: (r) => set({ results: r }),
      setAiConfig: (config) =>
        set((state) => ({ aiConfig: { ...state.aiConfig, ...config } })),
      addAiMessage: (msg) =>
        set((state) => ({ aiMessages: [...state.aiMessages, msg] })),
      updateAiMessage: (id, patch) =>
        set((state) => ({
          aiMessages: state.aiMessages.map((m) =>
            m.id === id ? { ...m, ...patch } : m,
          ),
        })),
      clearAiMessages: () => set({ aiMessages: [] }),
    }),
    {
      name: 'gtopt-gui-plus',
      partialize: (state) => ({
        caseData: state.caseData,
        theme: state.theme,
        aiConfig: state.aiConfig,
      }),
    },
  ),
);
