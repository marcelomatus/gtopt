import { describe, it, expect } from 'vitest';
import { getTable, findUidColumns, pivotByBlock } from '@/lib/results-tables';
import type { Table } from '@/lib/results-tables';

const sampleResults = {
  solution: { obj_value: '5.0', status: '0' },
  outputs: {
    'Generator/generation_sol': {
      columns: ['scenario', 'stage', 'block', 'uid:1', 'uid:2'],
      data: [
        [1, 1, 1, 100, 50],
        [1, 1, 2, 120, 30],
        [1, 1, 3, 80, 70],
      ],
    },
    'Bus/balance_dual': {
      columns: ['scenario', 'stage', 'block', 'uid:10'],
      data: [[1, 1, 1, 20.0]],
    },
  },
};

describe('getTable', () => {
  it('returns table for known path', () => {
    const t = getTable(sampleResults, 'Generator/generation_sol');
    expect(t).not.toBeNull();
    expect(t!.columns).toContain('uid:1');
    expect(t!.data).toHaveLength(3);
  });

  it('returns null for unknown path', () => {
    expect(getTable(sampleResults, 'Line/flowp_sol')).toBeNull();
    expect(getTable(null, 'any')).toBeNull();
  });

  it('returns null when structure is malformed', () => {
    expect(getTable({ outputs: { foo: { columns: null } } }, 'foo')).toBeNull();
  });
});

describe('findUidColumns', () => {
  it('extracts uid columns', () => {
    const cols = findUidColumns(['scenario', 'stage', 'block', 'uid:1', 'uid:2']);
    expect(cols).toHaveLength(2);
    expect(cols[0]).toEqual({ key: 'uid:1', uid: 1 });
    expect(cols[1]).toEqual({ key: 'uid:2', uid: 2 });
  });

  it('returns empty array when no uid columns', () => {
    expect(findUidColumns(['scenario', 'stage', 'block'])).toHaveLength(0);
  });

  it('handles negative uids', () => {
    const cols = findUidColumns(['uid:-1', 'uid:999']);
    expect(cols[0].uid).toBe(-1);
    expect(cols[1].uid).toBe(999);
  });
});

describe('pivotByBlock', () => {
  const table: Table = {
    columns: ['scenario', 'stage', 'block', 'uid:1', 'uid:2'],
    data: [
      [1, 1, 1, 100, 50],
      [1, 1, 2, 120, 30],
    ],
  };

  it('returns correct xKey, uids, and keys', () => {
    const { xKey, uids, keys } = pivotByBlock(table);
    expect(xKey).toBe('block');
    expect(uids).toEqual([1, 2]);
    expect(keys).toEqual(['uid:1', 'uid:2']);
  });

  it('maps rows correctly', () => {
    const { rows } = pivotByBlock(table);
    expect(rows).toHaveLength(2);
    expect(rows[0]['block']).toBe(1);
    expect(rows[0]['uid:1']).toBe(100);
    expect(rows[0]['uid:2']).toBe(50);
    expect(rows[1]['uid:1']).toBe(120);
  });

  it('falls back to stage when block column is absent', () => {
    const t: Table = {
      columns: ['scenario', 'stage', 'uid:1'],
      data: [[1, 2, 99]],
    };
    const { xKey } = pivotByBlock(t);
    expect(xKey).toBe('stage');
  });

  it('converts non-numeric values to 0', () => {
    const t: Table = {
      columns: ['block', 'uid:1'],
      data: [[1, 'bad_value']],
    };
    const { rows } = pivotByBlock(t);
    expect(rows[0]['uid:1']).toBe(0);
  });
});
