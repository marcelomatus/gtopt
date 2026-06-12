'use client';

import * as React from 'react';
import {
  BarChart,
  Bar,
  XAxis,
  YAxis,
  Tooltip,
  Cell,
  ResponsiveContainer,
} from 'recharts';
import { useStore } from '@/lib/store';
import { getTable, findUidColumns } from '@/lib/results-tables';
import { inferTech, techColor, TECH_LABELS } from '@/lib/tech-colors';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';

const DEFAULT_FACTORS: Record<string, number> = {
  hydro: 10,
  solar: 20,
  wind: 15,
  thermal: 700,
  battery: 0,
  other: 400,
};

const FACTOR_ORDER = ['thermal', 'hydro', 'solar', 'wind', 'battery', 'other'];

function formatTCO2(v: number): string {
  if (v >= 1e6) return `${(v / 1e6).toFixed(2)} MtCO₂`;
  if (v >= 1e3) return `${(v / 1e3).toFixed(1)} ktCO₂`;
  return `${v.toFixed(0)} tCO₂`;
}

export function EmissionsPanel({
  results,
}: {
  results: Record<string, unknown> | null;
}) {
  const caseData = useStore((s) => s.caseData);

  const [factors, setFactors] = React.useState<Record<string, number>>(
    DEFAULT_FACTORS,
  );

  const setFactor = React.useCallback((tech: string, value: number) => {
    setFactors((prev) => ({ ...prev, [tech]: value }));
  }, []);

  const table = getTable(results, 'Generator/generation_sol');

  const { chartData, totalTCO2 } = React.useMemo(() => {
    if (!table) return { chartData: [], totalTCO2: 0 };

    const gens = (caseData.system?.generator ?? []) as Array<{
      uid?: number;
      name?: string;
    }>;
    const uidCols = findUidColumns(table.columns);

    const techTotals: Record<string, number> = {};

    for (const { key, uid } of uidCols) {
      const gen = gens.find((g) => Number(g.uid) === uid);
      const tech = inferTech(gen?.name);
      const colIdx = table.columns.indexOf(key);

      let totalGen = 0;
      for (const row of table.data) {
        const v = row[colIdx];
        totalGen += typeof v === 'number' ? v : Number(v) || 0;
      }

      const factor = factors[tech] ?? factors['other'] ?? 400;
      const emissionsG = totalGen * factor;
      techTotals[tech] = (techTotals[tech] ?? 0) + emissionsG;
    }

    const data = Object.entries(techTotals)
      .filter(([, g]) => g > 0)
      .map(([tech, gCO2]) => ({
        tech,
        label: TECH_LABELS[tech] ?? tech,
        tCO2: gCO2 / 1e6,
        color: techColor(tech),
      }))
      .sort(
        (a, b) =>
          FACTOR_ORDER.indexOf(a.tech) - FACTOR_ORDER.indexOf(b.tech),
      );

    const total = data.reduce((sum, d) => sum + d.tCO2, 0);
    return { chartData: data, totalTCO2: total };
  }, [table, caseData, factors]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>Emissions estimate</CardTitle>
        <CardDescription>
          CO₂-equivalent estimates based on generation totals and editable
          intensity factors (gCO₂/MWh).
        </CardDescription>
      </CardHeader>
      <CardContent className="space-y-4">
        {!results ? (
          <div className="flex h-40 items-center justify-center text-sm text-muted-foreground">
            No results loaded. Upload a results ZIP to compute emissions
            estimates.
          </div>
        ) : (
          <>
            {/* KPI tile */}
            <div className="inline-flex items-baseline gap-2 rounded-lg border bg-card p-4 shadow-sm">
              <span className="text-3xl font-bold tabular-nums">
                {formatTCO2(totalTCO2)}
              </span>
              <span className="text-sm text-muted-foreground">total CO₂-eq</span>
            </div>

            {chartData.length === 0 ? (
              <div className="flex h-40 items-center justify-center text-sm text-muted-foreground">
                No{' '}
                <code className="mx-1">Generator/generation_sol</code> table in
                results.
              </div>
            ) : (
              <div className="h-[220px]">
                <ResponsiveContainer width="100%" height="100%">
                  <BarChart data={chartData} barCategoryGap="30%">
                    <XAxis dataKey="label" fontSize={12} />
                    <YAxis
                      fontSize={12}
                      tickFormatter={(v: number) =>
                        v >= 1000 ? `${(v / 1000).toFixed(0)}k` : String(v)
                      }
                      label={{
                        value: 'tCO₂',
                        angle: -90,
                        position: 'insideLeft',
                        fontSize: 11,
                      }}
                    />
                    <Tooltip
                      formatter={(v: unknown) =>
                        typeof v === 'number' ? [formatTCO2(v), 'tCO₂-eq'] : [String(v)]
                      }
                    />
                    <Bar dataKey="tCO2" name="tCO₂-eq" radius={[4, 4, 0, 0]}>
                      {chartData.map((d) => (
                        <Cell key={d.tech} fill={d.color} />
                      ))}
                    </Bar>
                  </BarChart>
                </ResponsiveContainer>
              </div>
            )}

            {/* Editable factors table */}
            <div>
              <p className="mb-2 text-xs font-medium uppercase tracking-wide text-muted-foreground">
                Emission intensity factors (gCO₂/MWh)
              </p>
              <div className="grid grid-cols-2 gap-2 sm:grid-cols-3 lg:grid-cols-6">
                {FACTOR_ORDER.map((tech) => (
                  <div
                    key={tech}
                    className="flex flex-col gap-1 rounded-md border bg-card p-2"
                  >
                    <label
                      htmlFor={`factor-${tech}`}
                      className="flex items-center gap-1 text-xs font-medium"
                    >
                      <span
                        className="inline-block h-2 w-2 rounded-full"
                        style={{ background: techColor(tech) }}
                      />
                      {TECH_LABELS[tech] ?? tech}
                    </label>
                    <input
                      id={`factor-${tech}`}
                      type="number"
                      min={0}
                      step={1}
                      className="w-full rounded border border-input bg-background px-2 py-1 text-sm focus:outline-none focus:ring-1 focus:ring-ring"
                      value={factors[tech] ?? 0}
                      onChange={(e) => {
                        const val = parseFloat(e.target.value);
                        if (!Number.isNaN(val)) setFactor(tech, val);
                      }}
                    />
                  </div>
                ))}
              </div>
            </div>
          </>
        )}
      </CardContent>
    </Card>
  );
}
