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
import { getTable } from '@/lib/results-tables';

const CONVERGENCE_PATHS = [
  'sddp/convergence',
  'SDDP/convergence',
  'solve/sddp_convergence',
];

export function SddpConvergence({
  results,
}: {
  results: Record<string, unknown> | null;
}) {
  let table = null;
  for (const p of CONVERGENCE_PATHS) {
    const t = getTable(results, p);
    if (t) {
      table = t;
      break;
    }
  }

  const rows = React.useMemo(() => {
    if (!table) return [];
    const { columns, data } = table;
    return data.map((row) => {
      const o: Record<string, number> = {};
      columns.forEach((c, i) => {
        o[c] = Number(row[i]) || 0;
      });
      return o;
    });
  }, [table]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>SDDP convergence</CardTitle>
        <CardDescription>
          Upper / lower bound per iteration; gap % as second axis.
        </CardDescription>
      </CardHeader>
      <CardContent className="h-[360px]">
        {rows.length === 0 ? (
          <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
            No SDDP convergence table in results.
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={rows}>
              <CartesianGrid strokeOpacity={0.2} />
              <XAxis dataKey="iteration" fontSize={12} />
              <YAxis fontSize={12} />
              <Tooltip />
              <Legend />
              <Line type="monotone" dataKey="upper" stroke="hsl(0 72% 51%)" dot={false} name="upper" />
              <Line type="monotone" dataKey="lower" stroke="hsl(217 91% 45%)" dot={false} name="lower" />
            </LineChart>
          </ResponsiveContainer>
        )}
      </CardContent>
    </Card>
  );
}
