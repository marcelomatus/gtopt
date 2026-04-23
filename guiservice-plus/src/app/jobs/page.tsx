'use client';

import * as React from 'react';
import { useQuery } from '@tanstack/react-query';
import { api, type JobInfo } from '@/lib/api';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';

function statusVariant(
  s: string,
): 'default' | 'success' | 'destructive' | 'secondary' | 'warning' {
  const v = s.toLowerCase();
  if (['done', 'ok', 'finished'].includes(v)) return 'success';
  if (['error', 'failed'].includes(v)) return 'destructive';
  if (['running', 'solving', 'queued'].includes(v)) return 'warning';
  return 'secondary';
}

export default function JobsPage() {
  const { data, isLoading, isError } = useQuery({
    queryKey: ['jobs'],
    queryFn: () => api.listJobs(),
    refetchInterval: 3000,
  });

  const jobs: JobInfo[] = data?.jobs ?? [];

  return (
    <div className="space-y-4">
      <div>
        <h1 className="text-3xl font-semibold tracking-tight">Jobs</h1>
        <p className="text-muted-foreground">
          Track running and completed solver jobs (auto-refresh every 3 s).
        </p>
      </div>
      <Card>
        <CardHeader>
          <CardTitle>All jobs</CardTitle>
          <CardDescription>
            {isError
              ? 'Failed to reach the Flask guiservice.'
              : isLoading
                ? 'Loading…'
                : `${jobs.length} job${jobs.length === 1 ? '' : 's'}`}
          </CardDescription>
        </CardHeader>
        <CardContent>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead>Token</TableHead>
                <TableHead>Case</TableHead>
                <TableHead>Status</TableHead>
                <TableHead>Created</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {jobs.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={4} className="text-center text-muted-foreground">
                    No jobs yet.
                  </TableCell>
                </TableRow>
              ) : (
                jobs.map((j) => (
                  <TableRow key={j.token}>
                    <TableCell className="font-mono text-xs">
                      {j.token.slice(0, 12)}…
                    </TableCell>
                    <TableCell>{j.case_name ?? '—'}</TableCell>
                    <TableCell>
                      <Badge variant={statusVariant(String(j.status))}>
                        {String(j.status)}
                      </Badge>
                    </TableCell>
                    <TableCell className="text-sm text-muted-foreground">
                      {j.created ?? '—'}
                    </TableCell>
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </CardContent>
      </Card>
    </div>
  );
}
