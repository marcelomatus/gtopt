'use client';

import * as React from 'react';
import { useQuery } from '@tanstack/react-query';
import {
  Card,
  CardContent,
} from '@/components/ui/card';
import { api, type ResultsSummary } from '@/lib/api';
import { formatNumber } from '@/lib/utils';

function useSummary(results: Record<string, unknown> | null) {
  return useQuery<ResultsSummary>({
    queryKey: ['results-summary', results],
    queryFn: () => api.resultsSummary(results ?? {}),
    enabled: !!results,
  });
}

function Tile({ label, value, sub }: { label: string; value: string; sub?: string }) {
  return (
    <Card>
      <CardContent className="p-4">
        <div className="text-xs font-medium uppercase tracking-wide text-muted-foreground">
          {label}
        </div>
        <div className="kpi-number mt-1 text-xl">{value}</div>
        {sub && <div className="text-xs text-muted-foreground">{sub}</div>}
      </CardContent>
    </Card>
  );
}

export function KpiStrip({ results }: { results: Record<string, unknown> | null }) {
  const { data } = useSummary(results);
  if (!data) return null;
  return (
    <div className="grid grid-cols-2 gap-3 sm:grid-cols-3 lg:grid-cols-6">
      <Tile
        label="Status"
        value={String(data.status ?? '—')}
        sub={data.status === 0 || data.status === '0' ? 'Optimal' : 'Check solver log'}
      />
      <Tile
        label="Objective"
        value={formatNumber(data.obj_value ?? 0)}
        sub="Total cost"
      />
      <Tile
        label="Total gen"
        value={formatNumber(data.total_generation)}
        sub="MWh (or MW·Δt)"
      />
      <Tile
        label="Unserved"
        value={formatNumber(data.total_unserved)}
        sub={`peak ${formatNumber(data.peak_unserved)}`}
      />
      <Tile
        label="LMP range"
        value={`${formatNumber(data.lmp_min)} – ${formatNumber(data.lmp_max)}`}
        sub={`mean ${formatNumber(data.lmp_mean)}`}
      />
      <Tile
        label="Network"
        value={`${formatNumber(data.n_buses, 0)} bus / ${formatNumber(data.n_lines, 0)} line`}
        sub={`${formatNumber(data.n_generators, 0)} gen`}
      />
    </div>
  );
}
