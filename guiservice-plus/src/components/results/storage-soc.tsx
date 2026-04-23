'use client';

import * as React from 'react';
import {
  LineChart,
  Line,
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
import { getTable, pivotByBlock } from '@/lib/results-tables';
import { techColor } from '@/lib/tech-colors';
import { useStore } from '@/lib/store';

const SOC_CANDIDATES = [
  'Battery/soc_sol',
  'Battery/e_sol',
  'Battery/energy_sol',
];

export function StorageSoc({ results }: { results: Record<string, unknown> | null }) {
  const caseData = useStore((s) => s.caseData);
  let table = null;
  for (const path of SOC_CANDIDATES) {
    const t = getTable(results, path);
    if (t) {
      table = t;
      break;
    }
  }

  const { rows, xKey, series } = React.useMemo(() => {
    if (!table) return { rows: [], xKey: 'block', series: [] as { key: string; color: string; name: string }[] };
    const pivot = pivotByBlock(table);
    const batts = (caseData.system?.battery ?? []) as Array<{
      uid?: number;
      name?: string;
    }>;
    const series = pivot.keys.map((key, idx) => {
      const uid = pivot.uids[idx];
      const b = batts.find((x) => Number(x.uid) === uid);
      return {
        key,
        name: b?.name ?? `uid:${uid}`,
        color: techColor('battery'),
      };
    });
    return { rows: pivot.rows, xKey: pivot.xKey, series };
  }, [table, caseData]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>Battery state of charge</CardTitle>
        <CardDescription>
          Energy stored (MWh) per battery across blocks.
        </CardDescription>
      </CardHeader>
      <CardContent className="h-[360px]">
        {rows.length === 0 ? (
          <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
            No battery SoC output present.
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={rows}>
              <CartesianGrid strokeOpacity={0.2} />
              <XAxis dataKey={xKey} fontSize={12} />
              <YAxis fontSize={12} />
              <Tooltip />
              <Legend />
              {series.map((s, i) => (
                <Line
                  key={s.key}
                  type="monotone"
                  dataKey={s.key}
                  name={s.name}
                  stroke={techColor(i % 2 === 0 ? 'battery' : 'hydro')}
                  dot={false}
                />
              ))}
            </LineChart>
          </ResponsiveContainer>
        )}
      </CardContent>
    </Card>
  );
}
