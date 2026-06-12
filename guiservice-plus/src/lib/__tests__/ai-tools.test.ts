import { describe, it, expect, vi } from 'vitest';
import {
  executeToolCall,
  buildSystemPrompt,
  AI_TOOL_DEFINITIONS,
  type ToolContext,
} from '@/lib/ai-tools';

// ---- helpers ----

function makeCtx(overrides: Partial<ToolContext> = {}): ToolContext {
  const caseData = {
    case_name: 'test_case',
    options: {
      use_single_bus: true,
      scale_objective: 1000,
      method: 'monolithic',
      use_kirchhoff: false,
    },
    simulation: {
      block_array: [{ uid: 1, duration: 1 }, { uid: 2, duration: 2 }],
      stage_array: [{ uid: 1, first_block: 0, count_block: 2 }],
      scenario_array: [{ uid: 1, probability_factor: 1 }],
    },
    system: {
      bus: [
        { uid: 1, name: 'b1' },
        { uid: 2, name: 'b2' },
      ],
      generator: [
        { uid: 10, name: 'g_solar', bus: 'b1', pmax: 100, gcost: 0 },
        { uid: 11, name: 'g_thermal', bus: 'b2', pmax: 200, gcost: 30 },
      ],
      demand: [
        { uid: 20, name: 'd1', bus: 'b1', lmax: 150 },
        { uid: 21, name: 'd2', bus: 'b2', lmax: 100 },
      ],
    },
  };

  const updateCaseData = vi.fn((mutator: (d: typeof caseData) => typeof caseData) => {
    Object.assign(caseData, mutator(caseData));
  });

  return {
    caseData: caseData as never,
    results: null,
    updateCaseData: updateCaseData as never,
    ...overrides,
  };
}

// ---- tests ----

describe('AI_TOOL_DEFINITIONS', () => {
  it('exports 7 tool definitions', () => {
    expect(AI_TOOL_DEFINITIONS).toHaveLength(7);
  });

  it('all definitions have required fields', () => {
    for (const def of AI_TOOL_DEFINITIONS) {
      expect(def.type).toBe('function');
      expect(def.function.name).toBeTruthy();
      expect(def.function.description).toBeTruthy();
      expect(def.function.parameters).toBeTruthy();
    }
  });
});

describe('executeToolCall – get_case_info', () => {
  it('returns JSON with case summary', () => {
    const ctx = makeCtx();
    const result = executeToolCall('get_case_info', {}, ctx);
    const parsed = JSON.parse(result) as Record<string, unknown>;
    expect(parsed.case_name).toBe('test_case');
    expect((parsed.counts as Record<string, unknown>).bus).toBe(2);
    expect((parsed.counts as Record<string, unknown>).generator).toBe(2);
    expect((parsed.simulation as Record<string, unknown>).blocks).toBe(2);
    expect(parsed.results_loaded).toBe(false);
  });
});

describe('executeToolCall – get_elements', () => {
  it('returns generator elements as JSON', () => {
    const ctx = makeCtx();
    const result = executeToolCall('get_elements', { element_type: 'generator' }, ctx);
    const parsed = JSON.parse(result) as unknown[];
    expect(parsed).toHaveLength(2);
  });

  it('returns message for empty element type', () => {
    const ctx = makeCtx();
    const result = executeToolCall('get_elements', { element_type: 'battery' }, ctx);
    expect(result).toMatch(/No battery/i);
  });
});

describe('executeToolCall – scale_element_field', () => {
  it('scales all demand lmax by a factor', () => {
    const ctx = makeCtx();
    const result = executeToolCall(
      'scale_element_field',
      { element_type: 'demand', field: 'lmax', factor: 1.1 },
      ctx,
    );
    expect(result).toContain('2 demand');
    expect(ctx.updateCaseData).toHaveBeenCalled();
    // After the mutator ran, caseData.system.demand should have updated values
    const demands = ctx.caseData.system?.demand as Array<Record<string, unknown>>;
    expect(Number(demands[0].lmax)).toBeCloseTo(165, 1); // 150 * 1.1
    expect(Number(demands[1].lmax)).toBeCloseTo(110, 1); // 100 * 1.1
  });

  it('reports percentage correctly for decrease', () => {
    const ctx = makeCtx();
    const result = executeToolCall(
      'scale_element_field',
      { element_type: 'generator', field: 'gcost', factor: 0.9 },
      ctx,
    );
    expect(result).toContain('-10.0%');
  });

  it('skips non-numeric values', () => {
    const ctx = makeCtx();
    // gcost=0 is numeric — but gcost_str would be skipped
    ctx.caseData.system = {
      ...(ctx.caseData.system ?? {}),
      generator: [{ uid: 10, name: 'g1', bus: 'b1', pmax: 'pmax.parquet' }],
    };
    const result = executeToolCall(
      'scale_element_field',
      { element_type: 'generator', field: 'pmax', factor: 2 },
      ctx,
    );
    expect(result).toContain('skipped');
  });

  it('returns error for zero factor', () => {
    const result = executeToolCall(
      'scale_element_field',
      { element_type: 'demand', field: 'lmax', factor: 0 },
      makeCtx(),
    );
    expect(result).toContain('Error');
  });
});

describe('executeToolCall – update_element_field', () => {
  it('updates generator gcost by uid', () => {
    const ctx = makeCtx();
    const result = executeToolCall(
      'update_element_field',
      { element_type: 'generator', uid: 10, field: 'gcost', value: 50 },
      ctx,
    );
    expect(result).toContain('uid:10');
    expect(result).toContain('50');
    const gens = ctx.caseData.system?.generator as Array<Record<string, unknown>>;
    expect(gens[0].gcost).toBe(50);
  });

  it('updates element by name when uid not given', () => {
    const ctx = makeCtx();
    executeToolCall(
      'update_element_field',
      { element_type: 'demand', name: 'd1', field: 'lmax', value: 200 },
      ctx,
    );
    const demands = ctx.caseData.system?.demand as Array<Record<string, unknown>>;
    expect(demands[0].lmax).toBe(200);
  });

  it('returns error when element not found', () => {
    const result = executeToolCall(
      'update_element_field',
      { element_type: 'bus', uid: 999, field: 'name', value: 'x' },
      makeCtx(),
    );
    expect(result).toContain('No bus');
  });

  it('returns error when neither uid nor name given', () => {
    const result = executeToolCall(
      'update_element_field',
      { element_type: 'bus', field: 'name', value: 'x' },
      makeCtx(),
    );
    expect(result).toContain('Error');
  });
});

describe('executeToolCall – add_element', () => {
  it('adds a new bus', () => {
    const ctx = makeCtx();
    const result = executeToolCall(
      'add_element',
      { element_type: 'bus', element: { uid: 99, name: 'b99' } },
      ctx,
    );
    expect(result).toContain('b99');
    const buses = ctx.caseData.system?.bus as Array<Record<string, unknown>>;
    expect(buses).toHaveLength(3);
    expect(buses[2].uid).toBe(99);
  });

  it('auto-assigns uid when missing', () => {
    const ctx = makeCtx();
    executeToolCall(
      'add_element',
      { element_type: 'bus', element: { name: 'b_auto' } },
      ctx,
    );
    const buses = ctx.caseData.system?.bus as Array<Record<string, unknown>>;
    const added = buses.find((b) => b.name === 'b_auto');
    expect(added).toBeDefined();
    // uid should be > 2 (auto-increment)
    expect(Number(added?.uid)).toBeGreaterThan(2);
  });
});

describe('executeToolCall – remove_element', () => {
  it('removes element by uid', () => {
    const ctx = makeCtx();
    const result = executeToolCall(
      'remove_element',
      { element_type: 'bus', uid: 1 },
      ctx,
    );
    expect(result).toContain('uid:1');
    expect(ctx.caseData.system?.bus as unknown[]).toHaveLength(1);
  });

  it('returns error for unknown uid', () => {
    const result = executeToolCall(
      'remove_element',
      { element_type: 'bus', uid: 999 },
      makeCtx(),
    );
    expect(result).toContain('No bus');
  });
});

describe('executeToolCall – query_results', () => {
  it('returns error when results are null', () => {
    const result = executeToolCall(
      'query_results',
      { table_path: 'Bus/balance_dual', operation: 'max' },
      makeCtx(),
    );
    expect(result).toContain('No results table');
  });

  it('computes max LMP across uid columns', () => {
    const ctx = makeCtx({
      results: {
        outputs: {
          'Bus/balance_dual': {
            columns: ['scenario', 'stage', 'block', 'uid:1', 'uid:2'],
            data: [
              [1, 1, 1, 20, 25],
              [1, 1, 2, 30, 22],
            ],
          },
        },
      },
    });
    const result = executeToolCall(
      'query_results',
      { table_path: 'Bus/balance_dual', operation: 'max' },
      ctx,
    );
    expect(result).toContain('MAX');
    expect(result).toContain('uid:');
  });

  it('finds argmax uid column', () => {
    const ctx = makeCtx({
      results: {
        outputs: {
          'Bus/balance_dual': {
            columns: ['scenario', 'stage', 'block', 'uid:1', 'uid:2'],
            data: [[1, 1, 1, 5, 100]],
          },
        },
      },
    });
    const result = executeToolCall(
      'query_results',
      { table_path: 'Bus/balance_dual', operation: 'argmax' },
      ctx,
    );
    expect(result).toContain('uid: 2');
    expect(result).toContain('uid:2');
  });

  it('respects filter_stage', () => {
    const ctx = makeCtx({
      results: {
        outputs: {
          'Generator/generation_sol': {
            columns: ['scenario', 'stage', 'block', 'uid:1'],
            data: [
              [1, 1, 1, 100],
              [1, 2, 1, 200],
            ],
          },
        },
      },
    });
    const result = executeToolCall(
      'query_results',
      { table_path: 'Generator/generation_sol', operation: 'sum', filter_stage: 1 },
      ctx,
    );
    // Only stage=1 row should be included
    expect(result).toContain('1 rows');
  });
});

describe('executeToolCall – unknown tool', () => {
  it('returns helpful error message', () => {
    const result = executeToolCall('non_existent_tool', {}, makeCtx());
    expect(result).toContain('Unknown tool');
    expect(result).toContain('non_existent_tool');
  });
});

describe('buildSystemPrompt', () => {
  it('includes case name and element counts', () => {
    const ctx = makeCtx();
    const prompt = buildSystemPrompt(ctx.caseData, null);
    expect(prompt).toContain('test_case');
    expect(prompt).toContain('2 bus');
    expect(prompt).toContain('2 generator');
  });

  it('mentions results when loaded', () => {
    const ctx = makeCtx();
    const prompt = buildSystemPrompt(ctx.caseData, {
      solution: { status: '0', obj_value: '5.0' },
      outputs: {},
    });
    expect(prompt).toContain('Results are loaded');
    expect(prompt).toContain('status=0');
  });

  it('says no results when null', () => {
    const ctx = makeCtx();
    const prompt = buildSystemPrompt(ctx.caseData, null);
    expect(prompt).toContain('No results loaded');
  });
});
