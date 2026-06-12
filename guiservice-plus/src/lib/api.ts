/**
 * Typed wrappers around the Flask guiservice API.
 *
 * All requests are routed through the Next.js rewrite `/flask/*` which
 * proxies to the Flask guiservice (configurable via
 * `GTOPT_GUISERVICE_URL`).  This keeps the browser same-origin and
 * avoids any CORS issues.
 */

export const API_BASE = '/flask';

export type CaseData = {
  case_name?: string;
  options?: Record<string, unknown>;
  simulation?: {
    block_array?: Array<Record<string, unknown>>;
    stage_array?: Array<Record<string, unknown>>;
    scenario_array?: Array<Record<string, unknown>>;
    [key: string]: unknown;
  };
  system?: Record<string, Array<Record<string, unknown>>>;
  data_files?: Record<string, unknown>;
};

export type ValidationError = {
  type: string;
  element_type?: string;
  index?: number;
  field?: string;
  ref?: string;
  value?: unknown;
  uid?: number;
  name?: string;
  message: string;
};

export type ValidationResult = {
  ok: boolean;
  n_errors: number;
  n_warnings: number;
  errors: ValidationError[];
  warnings: ValidationError[];
};

export type TemplateEntry = {
  slug: string;
  name: string;
  description: string;
  category: string;
};

export type ResultsSummary = {
  status: string | number | null;
  obj_value: number | null;
  obj_value_raw: number | null;
  total_generation: number;
  generation_by_tech: Record<string, number>;
  total_cost_generation: number;
  total_load: number;
  total_unserved: number;
  peak_unserved: number;
  n_generators: number;
  n_buses: number;
  n_lines: number;
  lmp_min: number | null;
  lmp_max: number | null;
  lmp_mean: number | null;
  n_blocks: number;
};

export type JobInfo = {
  token: string;
  status: string;
  created?: string;
  case_name?: string;
  progress?: number;
  [key: string]: unknown;
};

async function apiFetch<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: {
      'Content-Type': 'application/json',
      ...(init?.headers ?? {}),
    },
    ...init,
  });
  if (!res.ok) {
    const text = await res.text();
    throw new Error(`API ${path} failed (${res.status}): ${text}`);
  }
  return (await res.json()) as T;
}

export const api = {
  // ------------------------- schemas -------------------------
  getSchemas: () => apiFetch<Record<string, unknown>>('/api/schemas'),
  getOptionsSchema: () => apiFetch<Record<string, unknown>>('/api/options_schema'),

  // ------------------------- templates -------------------------
  listTemplates: () =>
    apiFetch<{ templates: TemplateEntry[] }>('/api/templates'),
  getTemplate: (slug: string) =>
    apiFetch<CaseData>(`/api/templates/${encodeURIComponent(slug)}`),

  // ------------------------- case -------------------------
  validateCase: (caseData: CaseData) =>
    apiFetch<ValidationResult>('/api/case/validate', {
      method: 'POST',
      body: JSON.stringify(caseData),
    }),
  checkRefs: (caseData: CaseData) =>
    apiFetch<ValidationResult>('/api/case/check_refs', {
      method: 'POST',
      body: JSON.stringify(caseData),
    }),
  previewCase: (caseData: CaseData) =>
    apiFetch<{ planning: Record<string, unknown> }>('/api/case/preview', {
      method: 'POST',
      body: JSON.stringify(caseData),
    }),

  // ------------------------- results -------------------------
  resultsSummary: (
    results: Record<string, unknown>,
    options?: { scale_objective?: number; tech_map?: Record<string, string> },
  ) =>
    apiFetch<ResultsSummary>('/api/results/summary', {
      method: 'POST',
      body: JSON.stringify({ results, ...(options ?? {}) }),
    }),
  resultsAggregate: (
    table: { columns: string[]; data: unknown[][] },
    options?: { group_by?: string[]; aggregation?: 'sum' | 'mean' | 'max' | 'min' },
  ) =>
    apiFetch<{ rows: unknown[][]; columns: string[] }>('/api/results/aggregate', {
      method: 'POST',
      body: JSON.stringify({ table, ...(options ?? {}) }),
    }),

  // ------------------------- jobs -------------------------
  listJobs: () =>
    apiFetch<{ jobs: JobInfo[] }>('/api/solve/jobs'),
  jobStatus: (token: string) =>
    apiFetch<JobInfo>(`/api/solve/status/${encodeURIComponent(token)}`),
  jobMonitor: (token: string) =>
    apiFetch<Record<string, unknown>>(`/api/solve/monitor/${encodeURIComponent(token)}`),

  // ------------------------- topology -------------------------
  topologyReactflow: (
    caseData: CaseData,
    options?: { subsystem?: string; aggregate?: string; voltage_threshold?: number },
  ) =>
    apiFetch<{
      nodes: Array<{ id: string; type: string; position: { x: number; y: number }; data: Record<string, unknown> }>;
      edges: Array<{ id: string; source: string; target: string; label: string; type: string; animated: boolean; style: Record<string, unknown> }>;
      meta: { n_nodes: number; n_edges: number; format: string };
    }>('/api/diagram/topology', {
      method: 'POST',
      body: JSON.stringify({
        caseData,
        format: 'reactflow',
        subsystem: options?.subsystem ?? 'full',
        aggregate: options?.aggregate ?? 'auto',
        voltage_threshold: options?.voltage_threshold ?? 0,
      }),
    }),
};
