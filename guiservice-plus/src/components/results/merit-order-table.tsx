'use client';

import * as React from 'react';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';
import { useStore } from '@/lib/store';
import { inferTech, techColor, TECH_LABELS } from '@/lib/tech-colors';

type Row = {
  uid: number;
  name: string;
  bus: string;
  tech: string;
  pmax: number;
  gcost: number;
};

export function MeritOrderTable({
  results,
}: {
  results: Record<string, unknown> | null;
}) {
  const caseData = useStore((s) => s.caseData);
  void results; // reserved for future — could overlay dispatched MW

  const rows: Row[] = React.useMemo(() => {
    const gens = (caseData.system?.generator ?? []) as Array<Record<string, unknown>>;
    return gens
      .map((g) => {
        const pmax = typeof g.pmax === 'number' ? g.pmax : Number((g as { capacity?: number }).capacity ?? 0);
        const gcost = typeof g.gcost === 'number' ? g.gcost : 0;
        const name = String(g.name ?? '');
        return {
          uid: Number(g.uid ?? 0),
          name,
          bus: String(g.bus ?? ''),
          tech: inferTech(name),
          pmax: Number.isFinite(pmax) ? pmax : 0,
          gcost: Number.isFinite(gcost) ? gcost : 0,
        };
      })
      .sort((a, b) => a.gcost - b.gcost);
  }, [caseData]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>Merit order</CardTitle>
        <CardDescription>
          Generators sorted by <code>gcost</code> (ascending). Merit order
          dispatch is valid only when no network congestion is present.
        </CardDescription>
      </CardHeader>
      <CardContent>
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead>#</TableHead>
              <TableHead>Name</TableHead>
              <TableHead>Bus</TableHead>
              <TableHead>Tech</TableHead>
              <TableHead className="text-right">pmax</TableHead>
              <TableHead className="text-right">gcost</TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {rows.map((r, i) => (
              <TableRow key={r.uid}>
                <TableCell>{i + 1}</TableCell>
                <TableCell className="font-medium">{r.name}</TableCell>
                <TableCell>{r.bus}</TableCell>
                <TableCell>
                  <span
                    className="inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs"
                    style={{
                      background: `${techColor(r.tech)}22`,
                      color: techColor(r.tech),
                    }}
                  >
                    <span
                      className="inline-block h-2 w-2 rounded-full"
                      style={{ background: techColor(r.tech) }}
                    />
                    {TECH_LABELS[r.tech] ?? r.tech}
                  </span>
                </TableCell>
                <TableCell className="text-right tabular-nums">{r.pmax.toFixed(2)}</TableCell>
                <TableCell className="text-right tabular-nums">{r.gcost.toFixed(2)}</TableCell>
              </TableRow>
            ))}
            {rows.length === 0 && (
              <TableRow>
                <TableCell colSpan={6} className="text-center text-muted-foreground">
                  No generators defined.
                </TableCell>
              </TableRow>
            )}
          </TableBody>
        </Table>
      </CardContent>
    </Card>
  );
}
