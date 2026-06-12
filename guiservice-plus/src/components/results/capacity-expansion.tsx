'use client';

import * as React from 'react';
import {
  BarChart,
  Bar,
  XAxis,
  YAxis,
  Tooltip,
  ResponsiveContainer,
  Legend,
  CartesianGrid,
} from 'recharts';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { getTable } from '@/lib/results-tables';
import { techColor, inferTech } from '@/lib/tech-colors';
import { useStore } from '@/lib/store';

/**
 * Pull expansion capacity from Demand/capacost_sol (units per stage)
 * or from Generator/capacity_sol if present.  Fallback to the raw
 * Generator/generation_sol aggregated max when neither exists.
 */
export function CapacityExpansion({
  results,
}: {
  results: Record<string, unknown> | null;
}) {
  const caseData = useStore((s) => s.caseData);

  const table =
    getTable(results, 'Demand/capacost_sol') ??
    getTable(results, 'Generator/capacity_sol') ??
    getTable(results, 'Generator/generation_sol');

  const { rows, series } = React.useMemo(() => {
    if (!table) return { rows: [], series: [] };
    const { columns, data } = table;
    const stageI = columns.indexOf('stage');
    const uidCols = columns
      .map((c, i) => ({ c, i }))
      .filter((x) => /^uid:(-?\d+)$/.test(x.c));

    // Sum per stage per uid (capacity is already a scalar per stage,
    // but we sum defensively in case duplicates exist).
    const map = new Map<number, Record<string, number>>();
    for (const row of data) {
      const stage = Number(row[stageI]);
      if (!Number.isFinite(stage)) continue;
      const acc = map.get(stage) ?? { stage };
      for (const { c, i } of uidCols) {
        acc[c] = (acc[c] ?? 0) + Number(row[i]) || 0;
      }
      map.set(stage, acc);
    }
    const rows = Array.from(map.values()).sort(
      (a, b) => Number(a.stage) - Number(b.stage),
    );
    const gens = (caseData.system?.generator ?? []) as Array<{
      uid?: number;
      name?: string;
    }>;
    const series = uidCols.map(({ c }) => {
      const uid = Number(c.replace('uid:', ''));
      const gen = gens.find((g) => Number(g.uid) === uid);
      const tech = inferTech(gen?.name);
      return { key: c, name: gen?.name ?? c, color: techColor(tech) };
    });
    return { rows, series };
  }, [table, caseData]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>Capacity by stage</CardTitle>
        <CardDescription>
          Capacity values aggregated per stage; useful for multi-stage
          expansion cases.
        </CardDescription>
      </CardHeader>
      <CardContent className="h-[360px]">
        {rows.length === 0 ? (
          <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
            No capacity output found.
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={rows}>
              <CartesianGrid strokeOpacity={0.2} />
              <XAxis dataKey="stage" fontSize={12} />
              <YAxis fontSize={12} />
              <Tooltip />
              <Legend />
              {series.map((s) => (
                <Bar key={s.key} dataKey={s.key} name={s.name} fill={s.color} />
              ))}
            </BarChart>
          </ResponsiveContainer>
        )}
      </CardContent>
    </Card>
  );
}
