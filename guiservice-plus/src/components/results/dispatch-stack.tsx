'use client';

import * as React from 'react';
import {
  AreaChart,
  Area,
  XAxis,
  YAxis,
  Tooltip,
  ResponsiveContainer,
  Legend,
  CartesianGrid,
} from 'recharts';
import { useStore } from '@/lib/store';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { getTable, pivotByBlock } from '@/lib/results-tables';
import { techColor, inferTech, TECH_LABELS } from '@/lib/tech-colors';

export function DispatchStack({
  results,
}: {
  results: Record<string, unknown> | null;
}) {
  const caseData = useStore((s) => s.caseData);
  const table = getTable(results, 'Generator/generation_sol');

  const { rows, xKey, series } = React.useMemo(() => {
    if (!table) return { rows: [], xKey: 'block', series: [] as { key: string; tech: string; color: string; name: string }[] };
    const pivot = pivotByBlock(table);
    const gens = (caseData.system?.generator ?? []) as Array<{
      uid?: number;
      name?: string;
    }>;

    const series = pivot.keys.map((key, idx) => {
      const uid = pivot.uids[idx];
      const gen = gens.find((g) => Number(g.uid) === uid);
      const tech = inferTech(gen?.name);
      return {
        key,
        tech,
        color: techColor(tech),
        name: gen?.name ?? `uid:${uid}`,
      };
    });
    return { rows: pivot.rows, xKey: pivot.xKey, series };
  }, [table, caseData]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>Dispatch stack</CardTitle>
        <CardDescription>
          Generation in MW by unit, stacked. Colour coded by inferred technology.
          Legend:{' '}
          {Object.entries(TECH_LABELS).map(([k, v]) => (
            <span key={k} className="mr-3 inline-flex items-center gap-1">
              <span
                className="inline-block h-2 w-2 rounded-full"
                style={{ background: techColor(k) }}
              />
              {v}
            </span>
          ))}
        </CardDescription>
      </CardHeader>
      <CardContent className="h-[420px]">
        {rows.length === 0 ? (
          <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
            No <code>Generator/generation_sol</code> table in results.
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <AreaChart data={rows}>
              <CartesianGrid strokeOpacity={0.2} />
              <XAxis dataKey={xKey} fontSize={12} />
              <YAxis fontSize={12} />
              <Tooltip />
              <Legend />
              {series.map((s) => (
                <Area
                  key={s.key}
                  type="monotone"
                  dataKey={s.key}
                  name={s.name}
                  stackId="gen"
                  stroke={s.color}
                  fill={s.color}
                  fillOpacity={0.7}
                />
              ))}
            </AreaChart>
          </ResponsiveContainer>
        )}
      </CardContent>
    </Card>
  );
}
