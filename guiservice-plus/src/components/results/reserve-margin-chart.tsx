'use client';

import * as React from 'react';
import {
  ComposedChart,
  Bar,
  Line,
  XAxis,
  YAxis,
  Tooltip,
  Legend,
  ResponsiveContainer,
  CartesianGrid,
} from 'recharts';
import { useStore } from '@/lib/store';
import { getTable, findUidColumns } from '@/lib/results-tables';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';

const RESERVE_PATHS = [
  'ReserveZone/up_reserve_sol',
  'ReserveZone/up_reserve_req',
  'ReserveZone/down_reserve_sol',
  'ReserveZone/down_reserve_req',
] as const;

export function ReserveMarginChart({
  results,
}: {
  results: Record<string, unknown> | null;
}) {
  const caseData = useStore((s) => s.caseData);
  const stages = (caseData.simulation?.stage_array ?? []) as Array<{
    uid?: number;
  }>;
  const stageUids = stages.map((s) => Number(s.uid ?? 0));

  const [selectedStage, setSelectedStage] = React.useState<number>(
    stageUids[0] ?? 1,
  );

  const { provTable, reqTable } = React.useMemo(() => {
    return {
      provTable:
        getTable(results, 'ReserveZone/up_reserve_sol') ??
        getTable(results, 'ReserveZone/down_reserve_sol'),
      reqTable:
        getTable(results, 'ReserveZone/up_reserve_req') ??
        getTable(results, 'ReserveZone/down_reserve_req'),
    };
  }, [results]);

  const hasData = provTable !== null || reqTable !== null;

  const chartData = React.useMemo(() => {
    const t = provTable ?? reqTable;
    if (!t) return [];

    const stageIdx = t.columns.indexOf('stage');
    const blockIdx = t.columns.indexOf('block');
    const uidCols = findUidColumns(t.columns);

    const rows = t.data.filter((row) => {
      if (stageIdx < 0) return true;
      return Number(row[stageIdx]) === selectedStage;
    });

    return rows.map((row) => {
      const xVal =
        blockIdx >= 0
          ? Number(row[blockIdx])
          : stageIdx >= 0
            ? Number(row[stageIdx])
            : 0;

      const entry: Record<string, number> = { x: xVal };

      if (provTable) {
        let sum = 0;
        for (const { key } of uidCols) {
          const i = t.columns.indexOf(key);
          sum += typeof row[i] === 'number' ? (row[i] as number) : Number(row[i]) || 0;
        }
        entry['provision'] = sum;
      }

      if (reqTable && reqTable !== t) {
        const ri = reqTable.columns.indexOf('block') >= 0
          ? reqTable.columns.indexOf('block')
          : reqTable.columns.indexOf('stage');
        const reqUids = findUidColumns(reqTable.columns);
        const reqRow = reqTable.data.find(
          (r) =>
            Number(r[ri]) === xVal &&
            (stageIdx < 0 || Number(r[stageIdx]) === selectedStage),
        );
        if (reqRow) {
          let reqSum = 0;
          for (const { key } of reqUids) {
            const i = reqTable.columns.indexOf(key);
            reqSum += typeof reqRow[i] === 'number' ? (reqRow[i] as number) : Number(reqRow[i]) || 0;
          }
          entry['requirement'] = reqSum;
        }
      } else if (reqTable === t) {
        // single table is the requirement
        let sum = 0;
        for (const { key } of uidCols) {
          const i = t.columns.indexOf(key);
          sum += typeof row[i] === 'number' ? (row[i] as number) : Number(row[i]) || 0;
        }
        entry['requirement'] = sum;
      }

      return entry;
    });
  }, [provTable, reqTable, selectedStage]);

  const availablePaths = RESERVE_PATHS.filter(
    (p) => getTable(results, p) !== null,
  );

  return (
    <Card>
      <CardHeader className="flex flex-row items-start justify-between gap-4">
        <div>
          <CardTitle>Reserve margin</CardTitle>
          <CardDescription>
            Up-reserve provision vs requirement per block.
            {availablePaths.length > 0 && (
              <span className="ml-1 text-xs">
                ({availablePaths.join(', ')})
              </span>
            )}
          </CardDescription>
        </div>
        {stageUids.length > 1 && (
          <select
            className="rounded border border-input bg-background px-2 py-1 text-sm"
            value={selectedStage}
            onChange={(e) => setSelectedStage(Number(e.target.value))}
          >
            {stageUids.map((uid) => (
              <option key={uid} value={uid}>
                Stage {uid}
              </option>
            ))}
          </select>
        )}
      </CardHeader>
      <CardContent className="h-[300px]">
        {!results || !hasData ? (
          <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
            No reserve data in results.
          </div>
        ) : chartData.length === 0 ? (
          <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
            No data for stage {selectedStage}.
          </div>
        ) : (
          <ResponsiveContainer width="100%" height="100%">
            <ComposedChart data={chartData}>
              <CartesianGrid strokeOpacity={0.2} />
              <XAxis dataKey="x" fontSize={12} label={{ value: 'block', position: 'insideBottom', offset: -2, fontSize: 11 }} />
              <YAxis fontSize={12} />
              <Tooltip />
              <Legend />
              {'provision' in (chartData[0] ?? {}) && (
                <Bar
                  dataKey="provision"
                  name="Provision (MW)"
                  fill="hsl(262 83% 58%)"
                  fillOpacity={0.7}
                  radius={[2, 2, 0, 0]}
                />
              )}
              {'requirement' in (chartData[0] ?? {}) && (
                <Line
                  type="stepAfter"
                  dataKey="requirement"
                  name="Requirement (MW)"
                  stroke="hsl(0 72% 51%)"
                  strokeWidth={2}
                  dot={false}
                />
              )}
            </ComposedChart>
          </ResponsiveContainer>
        )}
      </CardContent>
    </Card>
  );
}
