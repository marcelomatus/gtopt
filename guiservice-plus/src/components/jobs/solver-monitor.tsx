'use client';

/**
 * SolverMonitor — live SDDP upper/lower bound convergence chart.
 *
 * Polls /flask/api/jobs/:jobId/progress every 2 s while the job is running.
 * Shows a Recharts ComposedChart with two lines (upper bound dashed, lower
 * bound solid) and a shaded gap band between them.
 *
 * Stops polling when the gap drops below 0.1 % or when `isRunning` is false.
 */

import * as React from 'react';
import { useQuery } from '@tanstack/react-query';
import {
  ComposedChart,
  Line,
  Area,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
  ResponsiveContainer,
} from 'recharts';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

type ProgressPoint = {
  n: number;
  upper: number;
  lower: number;
};

type ProgressResponse = {
  iterations: ProgressPoint[];
};

// ---------------------------------------------------------------------------
// Helper — compute gap %
// ---------------------------------------------------------------------------

function gapPct(upper: number, lower: number): number {
  if (upper === 0) return 0;
  return Math.abs((upper - lower) / Math.abs(upper)) * 100;
}

// ---------------------------------------------------------------------------
// Component
// ---------------------------------------------------------------------------

export function SolverMonitor({
  jobId,
  isRunning,
}: {
  jobId: string;
  isRunning: boolean;
}) {
  const { data, isError, isFetching } = useQuery<ProgressResponse>({
    queryKey: ['job-progress', jobId],
    queryFn: async () => {
      const res = await fetch(`/flask/api/jobs/${encodeURIComponent(jobId)}/progress`);
      if (!res.ok) return { iterations: [] };
      return res.json() as Promise<ProgressResponse>;
    },
    refetchInterval: (query) => {
      if (!isRunning) return false;
      const pts = query.state.data?.iterations ?? [];
      if (pts.length > 0) {
        const last = pts[pts.length - 1];
        if (gapPct(last.upper, last.lower) < 0.1) return false;
      }
      return 2000;
    },
    enabled: isRunning,
  });

  const points = data?.iterations ?? [];

  // Build chart data with gap band
  const chartData = points.map((p) => ({
    n: p.n,
    upper: p.upper,
    lower: p.lower,
    gap: [p.lower, p.upper] as [number, number],
  }));

  const lastGap =
    points.length > 0
      ? gapPct(points[points.length - 1].upper, points[points.length - 1].lower)
      : null;

  if (!isRunning && points.length === 0) return null;

  return (
    <Card>
      <CardHeader className="flex flex-row items-center justify-between pb-2">
        <CardTitle className="text-sm font-medium">SDDP Convergence</CardTitle>
        <div className="flex items-center gap-2">
          {isFetching && isRunning && (
            <span className="text-xs text-muted-foreground">updating…</span>
          )}
          {lastGap !== null && (
            <Badge variant={lastGap < 1 ? 'success' : lastGap < 5 ? 'warning' : 'secondary'}>
              gap {lastGap.toFixed(2)}%
            </Badge>
          )}
        </div>
      </CardHeader>
      <CardContent>
        {isError ? (
          <p className="text-sm text-muted-foreground">
            Progress endpoint not available for this job.
          </p>
        ) : points.length === 0 ? (
          <p className="text-sm text-muted-foreground">Waiting for first iteration…</p>
        ) : (
          <ResponsiveContainer width="100%" height={240}>
            <ComposedChart data={chartData} margin={{ top: 4, right: 16, bottom: 4, left: 8 }}>
              <CartesianGrid strokeDasharray="3 3" opacity={0.3} />
              <XAxis dataKey="n" label={{ value: 'Iteration', position: 'insideBottom', offset: -2 }} tick={{ fontSize: 11 }} />
              <YAxis tick={{ fontSize: 11 }} tickFormatter={(v: number) => v.toFixed(1)} />
              <Tooltip
                formatter={(v: unknown, name: string) => [
                  typeof v === 'number' ? v.toFixed(2) : String(v),
                  name,
                ]}
              />
              <Legend verticalAlign="top" height={24} />
              {/* Gap band */}
              <Area
                dataKey="gap"
                fill="hsl(217 91% 60% / 0.15)"
                stroke="none"
                legendType="none"
                isAnimationActive={false}
              />
              {/* Upper bound (dashed) */}
              <Line
                type="monotone"
                dataKey="upper"
                name="Upper bound"
                stroke="hsl(0 72% 51%)"
                strokeWidth={2}
                strokeDasharray="5 3"
                dot={false}
                isAnimationActive={false}
              />
              {/* Lower bound (solid) */}
              <Line
                type="monotone"
                dataKey="lower"
                name="Lower bound"
                stroke="hsl(142 71% 45%)"
                strokeWidth={2}
                dot={false}
                isAnimationActive={false}
              />
            </ComposedChart>
          </ResponsiveContainer>
        )}
      </CardContent>
    </Card>
  );
}
