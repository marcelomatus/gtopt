'use client';

import * as React from 'react';
import Link from 'next/link';
import { useQuery } from '@tanstack/react-query';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { api, type JobInfo, type TemplateEntry } from '@/lib/api';
import { formatNumber } from '@/lib/utils';
import {
  Briefcase,
  FileEdit,
  Network,
  BarChart3,
  Sparkles,
  Zap,
} from 'lucide-react';

type KpiCardProps = {
  label: string;
  value: string;
  sub?: string;
  icon?: React.ReactNode;
  accent?: string;
};

function KpiCard({ label, value, sub, icon, accent }: KpiCardProps) {
  return (
    <Card>
      <CardContent className="flex items-start justify-between p-5">
        <div className="space-y-1">
          <div className="text-xs font-medium uppercase tracking-wide text-muted-foreground">
            {label}
          </div>
          <div className="kpi-number text-2xl text-foreground">{value}</div>
          {sub && <div className="text-xs text-muted-foreground">{sub}</div>}
        </div>
        {icon && (
          <div
            className={`flex h-10 w-10 items-center justify-center rounded-lg ${
              accent ?? 'bg-primary/10 text-primary'
            }`}
          >
            {icon}
          </div>
        )}
      </CardContent>
    </Card>
  );
}

function useJobsSafe() {
  return useQuery({
    queryKey: ['jobs'],
    queryFn: () => api.listJobs().catch(() => ({ jobs: [] as JobInfo[] })),
    refetchInterval: 5000,
  });
}

function useTemplatesSafe() {
  return useQuery({
    queryKey: ['templates'],
    queryFn: () =>
      api.listTemplates().catch(() => ({ templates: [] as TemplateEntry[] })),
    staleTime: 60_000,
  });
}

export default function DashboardPage() {
  const jobs = useJobsSafe();
  const templates = useTemplatesSafe();

  const recentJobs = (jobs.data?.jobs ?? []).slice(0, 5);
  const nActive = recentJobs.filter((j) =>
    ['queued', 'running', 'solving'].includes(String(j.status).toLowerCase()),
  ).length;

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-semibold tracking-tight">Dashboard</h1>
          <p className="text-muted-foreground">
            Quick situational awareness for your gtopt planning workspace.
          </p>
        </div>
        <div className="flex gap-2">
          <Button asChild variant="outline">
            <Link href="/case">
              <FileEdit className="mr-2 h-4 w-4" /> Edit case
            </Link>
          </Button>
          <Button asChild>
            <Link href="/topology">
              <Network className="mr-2 h-4 w-4" /> View topology
            </Link>
          </Button>
        </div>
      </div>

      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 lg:grid-cols-4">
        <KpiCard
          label="Active jobs"
          value={formatNumber(nActive, 0)}
          sub={`${recentJobs.length} total shown`}
          icon={<Briefcase className="h-5 w-5" />}
          accent="bg-amber-500/10 text-amber-500"
        />
        <KpiCard
          label="Templates"
          value={formatNumber(templates.data?.templates?.length ?? 0, 0)}
          sub="Bundled IEEE cases + blank"
          icon={<Sparkles className="h-5 w-5" />}
        />
        <KpiCard
          label="Quick start"
          value="Open case"
          sub="Edit in the case editor"
          icon={<FileEdit className="h-5 w-5" />}
          accent="bg-emerald-500/10 text-emerald-500"
        />
        <KpiCard
          label="Solver"
          value="gtopt"
          sub="C++26 LP/MIP engine"
          icon={<Zap className="h-5 w-5" />}
          accent="bg-indigo-500/10 text-indigo-500"
        />
      </div>

      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
        <Card className="lg:col-span-2">
          <CardHeader>
            <CardTitle>Recent jobs</CardTitle>
            <CardDescription>
              Updated every 5 s via <code>/api/solve/jobs</code>.
            </CardDescription>
          </CardHeader>
          <CardContent>
            {jobs.isLoading ? (
              <div className="text-sm text-muted-foreground">Loading…</div>
            ) : recentJobs.length === 0 ? (
              <div className="text-sm text-muted-foreground">
                No jobs yet. Submit one from the Case editor.
              </div>
            ) : (
              <ul className="divide-y">
                {recentJobs.map((j) => (
                  <li
                    key={j.token}
                    className="flex items-center justify-between py-2 text-sm"
                  >
                    <div>
                      <div className="font-medium">
                        {j.case_name ?? j.token.slice(0, 8)}
                      </div>
                      <div className="text-xs text-muted-foreground">
                        {j.token}
                      </div>
                    </div>
                    <Badge
                      variant={
                        String(j.status).toLowerCase() === 'done'
                          ? 'success'
                          : String(j.status).toLowerCase() === 'error'
                            ? 'destructive'
                            : 'secondary'
                      }
                    >
                      {j.status}
                    </Badge>
                  </li>
                ))}
              </ul>
            )}
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Templates</CardTitle>
            <CardDescription>
              Start from a bundled benchmark case.
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-2">
            {(templates.data?.templates ?? []).map((t) => (
              <Link
                key={t.slug}
                href={`/case?template=${encodeURIComponent(t.slug)}`}
                className="block rounded-md border p-3 text-sm hover:bg-accent/50"
              >
                <div className="font-medium">{t.name}</div>
                <div className="text-xs text-muted-foreground">
                  {t.description}
                </div>
              </Link>
            ))}
            {(templates.data?.templates ?? []).length === 0 && (
              <div className="text-sm text-muted-foreground">
                No templates available.
              </div>
            )}
          </CardContent>
        </Card>
      </div>

      <Card>
        <CardHeader>
          <CardTitle>Results at a glance</CardTitle>
          <CardDescription>
            Upload a results ZIP in the <Link href="/results" className="underline">Results</Link> page to populate the dispatch, LMP, and SoC panels.
          </CardDescription>
        </CardHeader>
        <CardContent>
          <Button asChild variant="outline">
            <Link href="/results">
              <BarChart3 className="mr-2 h-4 w-4" /> Open results
            </Link>
          </Button>
        </CardContent>
      </Card>
    </div>
  );
}
