/**
 * Tool definitions and client-side executors for the gtopt AI agent.
 *
 * Tools are split into two categories:
 *  - Read tools: return a string with data from the case/results (no mutations).
 *  - Write tools: mutate the case via `ctx.updateCaseData` and return a
 *    human-readable confirmation string.
 *
 * The route handler (`/api/ai/chat`) forwards the JSON Schema definitions to
 * the AI provider.  When the AI responds with a tool_call, the frontend
 * calls `executeToolCall` which dispatches to the right executor.
 */

import type { CaseData } from './api';

// ---------------------------------------------------------------------------
// Context passed to every executor
// ---------------------------------------------------------------------------

export type ToolContext = {
  caseData: CaseData;
  results: Record<string, unknown> | null;
  updateCaseData: (mutator: (draft: CaseData) => CaseData) => void;
};

// ---------------------------------------------------------------------------
// JSON-Schema tool definitions (sent to the AI provider)
// ---------------------------------------------------------------------------

export type ToolDefinition = {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: Record<string, unknown>;
  };
};

export const AI_TOOL_DEFINITIONS: ToolDefinition[] = [
  {
    type: 'function',
    function: {
      name: 'get_case_info',
      description:
        'Return a JSON summary of the current case: element counts, options, block/stage/scenario counts. Use this before making any changes to understand the current state.',
      parameters: { type: 'object', properties: {}, required: [] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_elements',
      description:
        'Return the full JSON array for a given element type (bus, generator, demand, line, battery). Use this to inspect element details before editing.',
      parameters: {
        type: 'object',
        properties: {
          element_type: {
            type: 'string',
            enum: ['bus', 'generator', 'demand', 'line', 'battery'],
            description: 'The element type to retrieve.',
          },
        },
        required: ['element_type'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'scale_element_field',
      description:
        'Multiply all numeric values of a field across every element of a given type by a scaling factor. For example: scale demand lmax by 1.1 to increase all demands by 10%. String values (file references) are skipped. Returns a summary of changes.',
      parameters: {
        type: 'object',
        properties: {
          element_type: {
            type: 'string',
            enum: ['bus', 'generator', 'demand', 'line', 'battery'],
          },
          field: {
            type: 'string',
            description: 'The field name to scale (e.g. lmax, pmax, gcost, emax).',
          },
          factor: {
            type: 'number',
            description: 'Multiplicative scaling factor (e.g. 1.1 = +10%, 0.9 = -10%).',
          },
        },
        required: ['element_type', 'field', 'factor'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'update_element_field',
      description:
        'Set a field to a new value on a specific element identified by its uid or name. Use get_elements first to confirm the uid.',
      parameters: {
        type: 'object',
        properties: {
          element_type: {
            type: 'string',
            enum: ['bus', 'generator', 'demand', 'line', 'battery'],
          },
          uid: { type: 'number', description: 'Element uid (preferred).' },
          name: { type: 'string', description: 'Element name (fallback if uid unknown).' },
          field: { type: 'string', description: 'Field name to update.' },
          value: { description: 'New value (number, string, boolean, or array).' },
        },
        required: ['element_type', 'field', 'value'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'add_element',
      description:
        'Add a new element to the case. The element object must include at least a unique uid and a name.',
      parameters: {
        type: 'object',
        properties: {
          element_type: {
            type: 'string',
            enum: ['bus', 'generator', 'demand', 'line', 'battery'],
          },
          element: {
            type: 'object',
            description: 'Element attributes (uid, name, and type-specific fields).',
          },
        },
        required: ['element_type', 'element'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'remove_element',
      description: 'Remove an element from the case by uid.',
      parameters: {
        type: 'object',
        properties: {
          element_type: {
            type: 'string',
            enum: ['bus', 'generator', 'demand', 'line', 'battery'],
          },
          uid: { type: 'number', description: 'uid of the element to remove.' },
        },
        required: ['element_type', 'uid'],
      },
    },
  },
  {
    type: 'function',
    function: {
      name: 'query_results',
      description:
        'Aggregate a results table and return statistics. Use operation="argmax" or "argmin" to identify which element (uid) has the highest/lowest value. Supports optional filtering by scenario, stage, or block.',
      parameters: {
        type: 'object',
        properties: {
          table_path: {
            type: 'string',
            description:
              'Results table path, e.g. "Bus/balance_dual", "Generator/generation_sol", "Line/flowp_sol".',
          },
          operation: {
            type: 'string',
            enum: ['max', 'min', 'mean', 'sum', 'argmax', 'argmin'],
            description:
              'Aggregation operation. "argmax"/"argmin" return the uid column with the extreme value.',
          },
          filter_stage: { type: 'number', description: 'Optional: restrict to a single stage.' },
          filter_block: { type: 'number', description: 'Optional: restrict to a single block.' },
          filter_scenario: {
            type: 'number',
            description: 'Optional: restrict to a single scenario.',
          },
        },
        required: ['table_path', 'operation'],
      },
    },
  },
];

// ---------------------------------------------------------------------------
// Executor helpers
// ---------------------------------------------------------------------------

type Element = Record<string, unknown>;

function getElements(caseData: CaseData, kind: string): Element[] {
  return ((caseData.system?.[kind] ?? []) as Element[]).map((e) => ({ ...e }));
}

function findElement(
  elements: Element[],
  uid?: number,
  name?: string,
): Element | undefined {
  if (uid !== undefined) return elements.find((e) => Number(e.uid) === uid);
  if (name !== undefined) return elements.find((e) => e.name === name);
  return undefined;
}

// ---------------------------------------------------------------------------
// Individual executors
// ---------------------------------------------------------------------------

function execGetCaseInfo(ctx: ToolContext): string {
  const { caseData } = ctx;
  const sys = caseData.system ?? {};
  const sim = caseData.simulation ?? {};
  const info = {
    case_name: caseData.case_name ?? 'unnamed',
    counts: Object.fromEntries(
      Object.entries(sys).map(([k, v]) => [k, (v as unknown[]).length]),
    ),
    simulation: {
      blocks: (sim.block_array ?? []).length,
      stages: (sim.stage_array ?? []).length,
      scenarios: (sim.scenario_array ?? []).length,
    },
    options: caseData.options ?? {},
    results_loaded: ctx.results !== null,
  };
  return JSON.stringify(info, null, 2);
}

function execGetElements(args: Record<string, unknown>, ctx: ToolContext): string {
  const kind = String(args.element_type ?? '');
  const elements = getElements(ctx.caseData, kind);
  if (elements.length === 0) return `No ${kind} elements in the case.`;
  return JSON.stringify(elements, null, 2);
}

function execScaleElementField(args: Record<string, unknown>, ctx: ToolContext): string {
  const kind = String(args.element_type ?? '');
  const field = String(args.field ?? '');
  const factor = Number(args.factor ?? 1);

  if (!kind || !field) return 'Error: element_type and field are required.';
  if (!Number.isFinite(factor) || factor === 0)
    return 'Error: factor must be a non-zero finite number.';

  let changed = 0;
  let skipped = 0;
  const details: string[] = [];

  ctx.updateCaseData((draft) => {
    const sys = draft.system ?? {};
    const arr = [...((sys[kind] ?? []) as Element[])];
    const updated = arr.map((el) => {
      const val = el[field];
      if (typeof val === 'number' && Number.isFinite(val)) {
        changed++;
        const newVal = val * factor;
        details.push(`uid:${el.uid ?? '?'} ${field}: ${val} → ${newVal.toFixed(4)}`);
        return { ...el, [field]: newVal };
      }
      skipped++;
      return el;
    });
    return { ...draft, system: { ...sys, [kind]: updated } };
  });

  if (changed === 0 && skipped === 0) return `No ${kind} elements found.`;
  const pct = ((factor - 1) * 100).toFixed(1);
  const sign = factor > 1 ? '+' : '';
  return (
    `Scaled \`${field}\` on ${changed} ${kind}(s) by ×${factor} (${sign}${pct}%).\n` +
    (skipped > 0 ? `${skipped} skipped (non-numeric / file reference).\n` : '') +
    (details.length > 0 ? '\nDetails:\n' + details.join('\n') : '')
  );
}

function execUpdateElementField(args: Record<string, unknown>, ctx: ToolContext): string {
  const kind = String(args.element_type ?? '');
  const field = String(args.field ?? '');
  const uid = args.uid !== undefined ? Number(args.uid) : undefined;
  const name = args.name !== undefined ? String(args.name) : undefined;
  const value = args.value;

  if (!kind || !field) return 'Error: element_type and field are required.';
  if (uid === undefined && name === undefined)
    return 'Error: provide uid or name to identify the element.';

  let found = false;
  let oldVal: unknown = undefined;

  ctx.updateCaseData((draft) => {
    const sys = draft.system ?? {};
    const arr = [...((sys[kind] ?? []) as Element[])];
    const idx = arr.findIndex((e) =>
      uid !== undefined ? Number(e.uid) === uid : e.name === name,
    );
    if (idx === -1) return draft;
    found = true;
    oldVal = arr[idx][field];
    arr[idx] = { ...arr[idx], [field]: value };
    return { ...draft, system: { ...sys, [kind]: arr } };
  });

  if (!found) {
    return `No ${kind} found with ${uid !== undefined ? `uid=${uid}` : `name="${name}"`}.`;
  }
  return (
    `Updated ${kind} (${uid !== undefined ? `uid:${uid}` : `name:${name}`}) ` +
    `field \`${field}\`: ${JSON.stringify(oldVal)} → ${JSON.stringify(value)}.`
  );
}

function execAddElement(args: Record<string, unknown>, ctx: ToolContext): string {
  const kind = String(args.element_type ?? '');
  const element = args.element as Element | undefined;

  if (!kind) return 'Error: element_type is required.';
  if (!element || typeof element !== 'object')
    return 'Error: element must be an object.';

  const uid = element.uid !== undefined ? Number(element.uid) : undefined;

  ctx.updateCaseData((draft) => {
    const sys = draft.system ?? {};
    const arr = [...((sys[kind] ?? []) as Element[])];
    // Auto-assign uid if missing
    const nextUid =
      uid !== undefined
        ? uid
        : arr.reduce((m, e) => Math.max(m, Number(e.uid ?? 0)), 0) + 1;
    arr.push({ uid: nextUid, ...element });
    return { ...draft, system: { ...sys, [kind]: arr } };
  });

  return `Added new ${kind} "${element.name ?? '(unnamed)'}" (uid:${uid ?? 'auto-assigned'}).`;
}

function execRemoveElement(args: Record<string, unknown>, ctx: ToolContext): string {
  const kind = String(args.element_type ?? '');
  const uid = Number(args.uid);

  if (!kind || !Number.isFinite(uid)) return 'Error: element_type and uid are required.';

  let found = false;

  ctx.updateCaseData((draft) => {
    const sys = draft.system ?? {};
    const arr = [...((sys[kind] ?? []) as Element[])];
    const idx = arr.findIndex((e) => Number(e.uid) === uid);
    if (idx === -1) return draft;
    found = true;
    arr.splice(idx, 1);
    return { ...draft, system: { ...sys, [kind]: arr } };
  });

  if (!found) return `No ${kind} with uid=${uid} found.`;
  return `Removed ${kind} uid:${uid}.`;
}

type ResultTable = { columns: string[]; data: unknown[][] };

function getResultTable(
  results: Record<string, unknown> | null,
  path: string,
): ResultTable | null {
  if (!results) return null;
  const outputs = (results.outputs ?? results) as Record<string, unknown>;
  const t = outputs?.[path] as ResultTable | undefined;
  if (!t || !Array.isArray(t.columns) || !Array.isArray(t.data)) return null;
  return t;
}

function execQueryResults(args: Record<string, unknown>, ctx: ToolContext): string {
  const tablePath = String(args.table_path ?? '');
  const operation = String(args.operation ?? 'max');
  const filterStage = args.filter_stage !== undefined ? Number(args.filter_stage) : null;
  const filterBlock = args.filter_block !== undefined ? Number(args.filter_block) : null;
  const filterScenario =
    args.filter_scenario !== undefined ? Number(args.filter_scenario) : null;

  if (!tablePath) return 'Error: table_path is required.';

  const table = getResultTable(ctx.results, tablePath);
  if (!table) {
    return `No results table found at path "${tablePath}". Is a results ZIP loaded?`;
  }

  const { columns, data } = table;

  // Apply row filters
  const iScen = columns.indexOf('scenario');
  const iStage = columns.indexOf('stage');
  const iBlock = columns.indexOf('block');

  let rows = data as unknown[][];
  if (filterScenario !== null && iScen >= 0)
    rows = rows.filter((r) => Number(r[iScen]) === filterScenario);
  if (filterStage !== null && iStage >= 0)
    rows = rows.filter((r) => Number(r[iStage]) === filterStage);
  if (filterBlock !== null && iBlock >= 0)
    rows = rows.filter((r) => Number(r[iBlock]) === filterBlock);

  if (rows.length === 0) return `No rows match the given filter in "${tablePath}".`;

  // Find uid columns
  const uidCols = columns
    .map((c, i) => ({ col: c, idx: i, uid: /^uid:(-?\d+)$/.exec(c)?.[1] }))
    .filter((c) => c.uid != null);

  if (uidCols.length === 0)
    return `Table "${tablePath}" has no uid:N columns to aggregate.`;

  // Compute per-uid numeric values
  const perUid: Record<string, number[]> = {};
  for (const row of rows) {
    for (const { col, idx } of uidCols) {
      const v = Number(row[idx]);
      if (Number.isFinite(v)) {
        if (!perUid[col]) perUid[col] = [];
        perUid[col].push(v);
      }
    }
  }

  // Apply operation
  const agg = (vals: number[]): number => {
    if (vals.length === 0) return 0;
    switch (operation) {
      case 'max':
        return Math.max(...vals);
      case 'min':
        return Math.min(...vals);
      case 'mean':
        return vals.reduce((a, b) => a + b, 0) / vals.length;
      case 'sum':
        return vals.reduce((a, b) => a + b, 0);
      default:
        return 0;
    }
  };

  if (operation === 'argmax' || operation === 'argmin') {
    // Find the uid column whose aggregate is extreme
    let extremeCol = '';
    let extremeVal = operation === 'argmax' ? -Infinity : Infinity;
    for (const [col, vals] of Object.entries(perUid)) {
      const v = vals.reduce((a, b) => a + b, 0) / vals.length; // mean across time
      if (operation === 'argmax' ? v > extremeVal : v < extremeVal) {
        extremeVal = v;
        extremeCol = col;
      }
    }
    const uidNum = /^uid:(-?\d+)$/.exec(extremeCol)?.[1] ?? '?';
    return (
      `${operation === 'argmax' ? 'Maximum' : 'Minimum'} value in "${tablePath}":\n` +
      `  Element uid: ${uidNum}\n` +
      `  Column: ${extremeCol}\n` +
      `  Mean value across ${rows.length} rows: ${extremeVal.toFixed(4)}`
    );
  }

  const result: Record<string, number> = {};
  for (const [col, vals] of Object.entries(perUid)) {
    result[col] = agg(vals);
  }

  const lines = Object.entries(result)
    .sort((a, b) => b[1] - a[1])
    .map(([col, val]) => `  ${col}: ${val.toFixed(4)}`);

  return (
    `${operation.toUpperCase()} of "${tablePath}" (${rows.length} rows, ${filterStage !== null ? `stage=${filterStage}` : 'all stages'}):\n` +
    lines.join('\n')
  );
}

// ---------------------------------------------------------------------------
// Public dispatcher
// ---------------------------------------------------------------------------

/**
 * Execute a tool call by name with the given parsed arguments.
 *
 * Returns a human-readable result string to include in the follow-up
 * message sent back to the AI.
 */
export function executeToolCall(
  name: string,
  args: Record<string, unknown>,
  ctx: ToolContext,
): string {
  switch (name) {
    case 'get_case_info':
      return execGetCaseInfo(ctx);
    case 'get_elements':
      return execGetElements(args, ctx);
    case 'scale_element_field':
      return execScaleElementField(args, ctx);
    case 'update_element_field':
      return execUpdateElementField(args, ctx);
    case 'add_element':
      return execAddElement(args, ctx);
    case 'remove_element':
      return execRemoveElement(args, ctx);
    case 'query_results':
      return execQueryResults(args, ctx);
    default:
      return `Unknown tool: "${name}". Available tools: ${AI_TOOL_DEFINITIONS.map((t) => t.function.name).join(', ')}.`;
  }
}

// ---------------------------------------------------------------------------
// System prompt builder
// ---------------------------------------------------------------------------

/**
 * Build the system prompt injected at the start of every AI conversation.
 * Includes a gtopt domain briefing + current case context.
 */
export function buildSystemPrompt(
  caseData: CaseData,
  results: Record<string, unknown> | null,
): string {
  const sys = caseData.system ?? {};
  const sim = caseData.simulation ?? {};

  const counts = Object.entries(sys)
    .map(([k, v]) => `${(v as unknown[]).length} ${k}(s)`)
    .join(', ');

  const opts = caseData.options ?? {};
  const optStr = [
    opts.use_single_bus ? 'copper-plate mode' : 'multi-bus',
    opts.use_kirchhoff ? 'Kirchhoff DC-OPF' : 'transport model',
    `method=${opts.method ?? 'monolithic'}`,
    `scale_objective=${opts.scale_objective ?? 1}`,
  ].join(', ');

  const resLine = results
    ? `Results are loaded (status=${(results.solution as Record<string, unknown>)?.status ?? '?'}).`
    : 'No results loaded yet.';

  return `You are an expert AI assistant integrated into **gtopt GUI Plus** — a power systems Generation and Transmission Expansion Planning (GTEP) workbench.

**Current case:** "${caseData.case_name ?? 'unnamed'}"
**System:** ${counts || 'empty'}
**Simulation:** ${(sim.block_array ?? []).length} block(s), ${(sim.stage_array ?? []).length} stage(s), ${(sim.scenario_array ?? []).length} scenario(s)
**Options:** ${optStr}
**${resLine}**

You have access to tools that can read and modify the case or query results.  Guidelines:
- Always call \`get_case_info\` or \`get_elements\` first when you need to inspect data before making changes.
- Confirm what you changed and why after each tool call.
- When scaling or editing, always state the before and after values.
- For results queries, explain the power-system meaning of the answer (e.g. "the bus with the highest LMP is also the most constrained node").
- Be concise. Avoid generic disclaimers — the user is an expert power-systems engineer.`;
}
