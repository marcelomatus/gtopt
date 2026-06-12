'use client';

import type { ValidationResult } from '@/lib/api';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { AlertTriangle, CheckCircle2, AlertCircle } from 'lucide-react';

export function CaseHealthPanel({
  validation,
  loading,
}: {
  validation?: ValidationResult;
  loading?: boolean;
}) {
  const nE = validation?.n_errors ?? 0;
  const nW = validation?.n_warnings ?? 0;

  return (
    <Card>
      <CardHeader>
        <CardTitle className="flex items-center gap-2">
          {nE === 0 && nW === 0 ? (
            <CheckCircle2 className="h-4 w-4 text-emerald-500" />
          ) : nE > 0 ? (
            <AlertCircle className="h-4 w-4 text-destructive" />
          ) : (
            <AlertTriangle className="h-4 w-4 text-amber-500" />
          )}
          Case health
        </CardTitle>
        <CardDescription>
          {loading
            ? 'Validating…'
            : nE === 0 && nW === 0
              ? 'All checks passed.'
              : `${nE} error${nE === 1 ? '' : 's'}, ${nW} warning${nW === 1 ? '' : 's'}`}
        </CardDescription>
      </CardHeader>
      <CardContent className="space-y-2 text-sm">
        {validation?.errors.map((e, i) => (
          <div
            key={`e-${i}`}
            className="rounded-md border border-destructive/30 bg-destructive/5 p-2 text-xs text-destructive"
          >
            <div className="font-medium">{e.type}</div>
            <div>{e.message}</div>
          </div>
        ))}
        {validation?.warnings.map((w, i) => (
          <div
            key={`w-${i}`}
            className="rounded-md border border-amber-500/30 bg-amber-500/5 p-2 text-xs text-amber-700 dark:text-amber-400"
          >
            <div className="font-medium">{w.type}</div>
            <div>{w.message}</div>
          </div>
        ))}
      </CardContent>
    </Card>
  );
}
