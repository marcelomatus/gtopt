'use client';

import * as React from 'react';
import { useStore } from '@/lib/store';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Plus, Trash2 } from 'lucide-react';
import type { ElementKind } from '@/lib/schemas';

const COLUMNS: Record<ElementKind, string[]> = {
  bus: ['uid', 'name', 'voltage'],
  generator: ['uid', 'name', 'bus', 'pmin', 'pmax', 'gcost', 'capacity'],
  demand: ['uid', 'name', 'bus', 'lmax', 'fail_cost'],
  line: [
    'uid',
    'name',
    'bus_a',
    'bus_b',
    'tmax_ab',
    'tmax_ba',
    'reactance',
    'resistance',
  ],
  battery: ['uid', 'name', 'bus', 'emax', 'emin', 'efficiency_charge', 'efficiency_discharge'],
};

export function ElementEditor({ kind }: { kind: ElementKind }) {
  const items = useStore(
    (s) => (s.caseData.system?.[kind] ?? []) as Record<string, unknown>[],
  );
  const updateCaseData = useStore((s) => s.updateCaseData);
  const cols = COLUMNS[kind];

  const addRow = () => {
    updateCaseData((draft) => {
      const system = draft.system ?? {};
      const arr = [...((system[kind] ?? []) as Record<string, unknown>[])];
      const nextUid = (arr.reduce((m, r) => Math.max(m, Number(r.uid ?? 0)), 0) ?? 0) + 1;
      arr.push({ uid: nextUid, name: `${kind}${nextUid}` });
      return { ...draft, system: { ...system, [kind]: arr } };
    });
  };

  const removeRow = (i: number) => {
    updateCaseData((draft) => {
      const system = draft.system ?? {};
      const arr = [...((system[kind] ?? []) as Record<string, unknown>[])];
      arr.splice(i, 1);
      return { ...draft, system: { ...system, [kind]: arr } };
    });
  };

  const updateCell = (i: number, key: string, raw: string) => {
    updateCaseData((draft) => {
      const system = draft.system ?? {};
      const arr = [...((system[kind] ?? []) as Record<string, unknown>[])];
      const n = Number(raw);
      const val: unknown =
        raw === ''
          ? undefined
          : Number.isFinite(n) && /^-?\d+(?:\.\d+)?$/.test(raw.trim())
            ? n
            : raw;
      const next = { ...arr[i] };
      if (val === undefined) delete next[key];
      else next[key] = val;
      arr[i] = next;
      return { ...draft, system: { ...system, [kind]: arr } };
    });
  };

  return (
    <div>
      <div className="mb-2 flex items-center justify-between">
        <div className="text-sm text-muted-foreground">
          {items.length} {kind}
          {items.length === 1 ? '' : 's'}
        </div>
        <Button size="sm" variant="outline" onClick={addRow}>
          <Plus className="mr-1 h-3.5 w-3.5" /> Add {kind}
        </Button>
      </div>
      <div className="rounded-md border">
        <Table>
          <TableHeader>
            <TableRow>
              {cols.map((c) => (
                <TableHead key={c}>{c}</TableHead>
              ))}
              <TableHead className="w-10" />
            </TableRow>
          </TableHeader>
          <TableBody>
            {items.length === 0 ? (
              <TableRow>
                <TableCell colSpan={cols.length + 1} className="text-center text-muted-foreground">
                  No {kind} yet — click "Add {kind}".
                </TableCell>
              </TableRow>
            ) : (
              items.map((r, i) => (
                <TableRow key={`${i}-${r.uid}`}>
                  {cols.map((c) => (
                    <TableCell key={c}>
                      <Input
                        className="h-8"
                        defaultValue={String(r[c] ?? '')}
                        onBlur={(e) => updateCell(i, c, e.target.value)}
                      />
                    </TableCell>
                  ))}
                  <TableCell>
                    <Button
                      variant="ghost"
                      size="icon"
                      className="h-8 w-8"
                      onClick={() => removeRow(i)}
                    >
                      <Trash2 className="h-3.5 w-3.5" />
                    </Button>
                  </TableCell>
                </TableRow>
              ))
            )}
          </TableBody>
        </Table>
      </div>
    </div>
  );
}
