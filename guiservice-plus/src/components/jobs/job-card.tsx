'use client';
/**
 * JobCard — an expandable card for a single solver job.
 */

import * as React from 'react';
import { CheckCircle, XCircle, Loader2, Clock, ChevronDown, ChevronUp } from 'lucide-react';
import { Card, CardContent } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';

export type JobInfo = {
  job_id: string;
  status: 'queued' | 'running' | 'done' | 'error';
  case_name?: string;
  started_at?: string;
  finished_at?: string;
  objective?: number;
  kappa?: number;
  message?: string;
};

function useElapsed(startedAt: string | undefined, active: boolean): string {
  const [elapsed, setElapsed] = React.useState('');
  React.useEffect(() => {
    if (!active || !startedAt) return;
    const update = () => {
      const diff = Date.now() - new Date(startedAt).getTime();
      const s = Math.floor(diff / 1000);
      setElapsed(`${Math.floor(s / 60)}m ${s % 60}s`);
    };
    update();
    const id = setInterval(update, 1000);
    return () => clearInterval(id);
  }, [active, startedAt]);
  return elapsed;
}

function StatusIcon({ status }: { status: JobInfo['status'] }) {
  if (status === 'done') return <CheckCircle className="h-4 w-4 text-green-500" />;
  if (status === 'error') return <XCircle className="h-4 w-4 text-red-500" />;
  if (status === 'running') return <Loader2 className="h-4 w-4 animate-spin text-blue-500" />;
  return <Clock className="h-4 w-4 text-yellow-500" />;
}

function statusVariant(s: string): 'default' | 'success' | 'destructive' | 'secondary' | 'warning' {
  if (s === 'done') return 'success';
  if (s === 'error') return 'destructive';
  if (s === 'running' || s === 'queued') return 'warning';
  return 'secondary';
}

export function JobCard({
  job,
  onLoadResults,
  onCancel,
}: {
  job: JobInfo;
  onLoadResults?: (jobId: string) => void;
  onCancel?: (jobId: string) => void;
}) {
  const [expanded, setExpanded] = React.useState(false);
  const isRunning = job.status === 'running';
  const elapsed = useElapsed(job.started_at, isRunning);

  return (
    <Card className="mb-2">
      <CardContent className="p-3">
        <div className="flex items-center gap-2">
          <StatusIcon status={job.status} />
          <span className="flex-1 truncate font-medium text-sm">{job.case_name ?? job.job_id}</span>
          <Badge variant={statusVariant(job.status)} className="text-xs">
            {job.status}
          </Badge>
          <Button
            variant="ghost"
            size="icon"
            className="h-6 w-6"
            onClick={() => setExpanded((v) => !v)}
          >
            {expanded ? <ChevronUp className="h-3 w-3" /> : <ChevronDown className="h-3 w-3" />}
          </Button>
        </div>

        {isRunning && elapsed && (
          <div className="mt-1 text-xs text-muted-foreground">Elapsed: {elapsed}</div>
        )}

        {expanded && (
          <div className="mt-2 space-y-1 border-t pt-2 text-xs text-muted-foreground">
            <div>
              <span className="font-mono">ID:</span> {job.job_id}
            </div>
            {job.started_at && <div>Started: {new Date(job.started_at).toLocaleString()}</div>}
            {job.finished_at && <div>Finished: {new Date(job.finished_at).toLocaleString()}</div>}
            {job.objective !== undefined && (
              <div>Objective: {job.objective.toFixed(4)}</div>
            )}
            {job.kappa !== undefined && <div>Iterations: {job.kappa}</div>}
            {job.message && (
              <div className="rounded bg-muted p-1 font-mono text-[10px]">{job.message}</div>
            )}
            <div className="flex gap-2 pt-1">
              {job.status === 'done' && onLoadResults && (
                <Button size="sm" variant="default" className="h-6 text-xs" onClick={() => onLoadResults(job.job_id)}>
                  Load results
                </Button>
              )}
              {(job.status === 'queued' || job.status === 'running') && onCancel && (
                <Button size="sm" variant="destructive" className="h-6 text-xs" onClick={() => onCancel(job.job_id)}>
                  Cancel
                </Button>
              )}
            </div>
          </div>
        )}
      </CardContent>
    </Card>
  );
}
