/**
 * Helpers for extracting tabular data from the parsed `results` payload
 * returned by the Flask guiservice.  The structure is:
 *   {
 *     solution: { obj_value: string, status: string, kappa?: string },
 *     outputs: { "Generator/generation_sol": { columns, data }, ... }
 *   }
 */

export type Table = { columns: string[]; data: unknown[][] };

export function getTable(
  results: Record<string, unknown> | null,
  path: string,
): Table | null {
  if (!results) return null;
  const outputs = (results.outputs ?? results) as Record<string, unknown>;
  const t = outputs?.[path] as Table | undefined;
  if (!t || !t.columns || !t.data) return null;
  return t;
}

export function findUidColumns(columns: string[]): { key: string; uid: number }[] {
  const out: { key: string; uid: number }[] = [];
  for (const c of columns) {
    const m = /^uid:(-?\d+)$/.exec(c);
    if (m) out.push({ key: c, uid: Number(m[1]) });
  }
  return out;
}

/**
 * Pivot long-format rows into "one series per uid column" X/Y shape,
 * keyed by the first index column present (block > stage > scenario).
 */
export function pivotByBlock(table: Table): {
  xKey: string;
  uids: number[];
  keys: string[];
  rows: Array<Record<string, number | string>>;
} {
  const { columns, data } = table;
  const uidCols = findUidColumns(columns);
  const idxCol =
    columns.find((c) => c === 'block') ??
    columns.find((c) => c === 'stage') ??
    columns[0];
  const iIdx = columns.indexOf(idxCol);
  const rows: Array<Record<string, number | string>> = [];
  for (const row of data) {
    const o: Record<string, number | string> = { [idxCol]: row[iIdx] as number };
    for (const { key } of uidCols) {
      const i = columns.indexOf(key);
      const v = row[i];
      o[key] = typeof v === 'number' ? v : Number(v) || 0;
    }
    rows.push(o);
  }
  return {
    xKey: idxCol,
    uids: uidCols.map((c) => c.uid),
    keys: uidCols.map((c) => c.key),
    rows,
  };
}
