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

type Section = 'block_array' | 'stage_array' | 'scenario_array';

const COLUMNS: Record<Section, { key: string; label: string }[]> = {
  block_array: [
    { key: 'uid', label: 'uid' },
    { key: 'duration', label: 'duration (h)' },
  ],
  stage_array: [
    { key: 'uid', label: 'uid' },
    { key: 'first_block', label: 'first_block' },
    { key: 'count_block', label: 'count_block' },
    { key: 'active', label: 'active' },
  ],
  scenario_array: [
    { key: 'uid', label: 'uid' },
    { key: 'probability_factor', label: 'probability' },
    { key: 'active', label: 'active' },
  ],
};

const DEFAULTS: Record<Section, Record<string, unknown>> = {
  block_array: { uid: 0, duration: 1 },
  stage_array: { uid: 0, first_block: 0, count_block: 1, active: 1 },
  scenario_array: { uid: 0, probability_factor: 1, active: 1 },
};

export function SimulationEditor() {
  return (
    <div className="space-y-6">
      <SubTable section="block_array" title="Blocks" />
      <SubTable section="stage_array" title="Stages" />
      <SubTable section="scenario_array" title="Scenarios" />
    </div>
  );
}

function SubTable({ section, title }: { section: Section; title: string }) {
  const rows = useStore(
    (s) => (s.caseData.simulation?.[section] ?? []) as Record<string, unknown>[],
  );
  const updateCaseData = useStore((s) => s.updateCaseData);
  const cols = COLUMNS[section];

  const addRow = () => {
    updateCaseData((draft) => {
      const sim = draft.simulation ?? {};
      const arr = [...((sim[section] ?? []) as Record<string, unknown>[])];
      const nextUid = arr.reduce((m, r) => Math.max(m, Number(r.uid ?? 0)), 0) + 1;
      arr.push({ ...DEFAULTS[section], uid: nextUid });
      return { ...draft, simulation: { ...sim, [section]: arr } };
    });
  };

  const removeRow = (i: number) => {
    updateCaseData((draft) => {
      const sim = draft.simulation ?? {};
      const arr = [...((sim[section] ?? []) as Record<string, unknown>[])];
      arr.splice(i, 1);
      return { ...draft, simulation: { ...sim, [section]: arr } };
    });
  };

  const updateCell = (i: number, key: string, value: string) => {
    updateCaseData((draft) => {
      const sim = draft.simulation ?? {};
      const arr = [...((sim[section] ?? []) as Record<string, unknown>[])];
      const num = Number(value);
      const coerced = Number.isFinite(num) && value.trim() !== '' ? num : value;
      arr[i] = { ...arr[i], [key]: coerced };
      return { ...draft, simulation: { ...sim, [section]: arr } };
    });
  };

  return (
    <div>
      <div className="mb-2 flex items-center justify-between">
        <h3 className="text-sm font-semibold">{title}</h3>
        <Button size="sm" variant="outline" onClick={addRow}>
          <Plus className="mr-1 h-3.5 w-3.5" /> Add row
        </Button>
      </div>
      <div className="rounded-md border">
        <Table>
          <TableHeader>
            <TableRow>
              {cols.map((c) => (
                <TableHead key={c.key}>{c.label}</TableHead>
              ))}
              <TableHead className="w-10" />
            </TableRow>
          </TableHeader>
          <TableBody>
            {rows.length === 0 ? (
              <TableRow>
                <TableCell colSpan={cols.length + 1} className="text-center text-muted-foreground">
                  No rows
                </TableCell>
              </TableRow>
            ) : (
              rows.map((r, i) => (
                <TableRow key={i}>
                  {cols.map((c) => (
                    <TableCell key={c.key}>
                      <Input
                        className="h-8"
                        defaultValue={String(r[c.key] ?? '')}
                        onBlur={(e) => updateCell(i, c.key, e.target.value)}
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
