'use client';

import * as React from 'react';
import { useStore } from '@/lib/store';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import {
  Tabs,
  TabsContent,
  TabsList,
  TabsTrigger,
} from '@/components/ui/tabs';
import { Button } from '@/components/ui/button';
import { DispatchStack } from '@/components/results/dispatch-stack';
import { StorageSoc } from '@/components/results/storage-soc';
import { CapacityExpansion } from '@/components/results/capacity-expansion';
import { SddpConvergence } from '@/components/results/sddp-convergence';
import { MeritOrderTable } from '@/components/results/merit-order-table';
import { KpiStrip } from '@/components/results/kpi-strip';

export default function ResultsPage() {
  const results = useStore((s) => s.results);
  const setResults = useStore((s) => s.setResults);

  const onFile = async (file: File) => {
    const formData = new FormData();
    formData.append('file', file);
    const res = await fetch('/flask/api/results/upload', {
      method: 'POST',
      body: formData,
    });
    if (!res.ok) {
      alert('Failed to upload results');
      return;
    }
    const data = await res.json();
    setResults(data);
  };

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-semibold tracking-tight">Results</h1>
          <p className="text-muted-foreground">
            Upload a gtopt results ZIP to populate the dashboards below.
          </p>
        </div>
        <div className="flex items-center gap-2">
          <input
            id="results-file"
            type="file"
            accept=".zip,application/zip"
            className="hidden"
            onChange={(e) => {
              const f = e.target.files?.[0];
              if (f) onFile(f);
            }}
          />
          <Button asChild variant="outline">
            <label htmlFor="results-file" className="cursor-pointer">
              Upload results ZIP
            </label>
          </Button>
        </div>
      </div>

      {!results ? (
        <Card>
          <CardHeader>
            <CardTitle>No results loaded</CardTitle>
            <CardDescription>
              Once a solver job completes, upload the results ZIP to explore
              dispatch, LMP, storage, and expansion insights.
            </CardDescription>
          </CardHeader>
        </Card>
      ) : (
        <>
          <KpiStrip results={results} />
          <Tabs defaultValue="dispatch">
            <TabsList>
              <TabsTrigger value="dispatch">Dispatch</TabsTrigger>
              <TabsTrigger value="storage">Storage SoC</TabsTrigger>
              <TabsTrigger value="expansion">Expansion</TabsTrigger>
              <TabsTrigger value="sddp">SDDP convergence</TabsTrigger>
              <TabsTrigger value="merit">Merit order</TabsTrigger>
            </TabsList>
            <TabsContent value="dispatch">
              <DispatchStack results={results} />
            </TabsContent>
            <TabsContent value="storage">
              <StorageSoc results={results} />
            </TabsContent>
            <TabsContent value="expansion">
              <CapacityExpansion results={results} />
            </TabsContent>
            <TabsContent value="sddp">
              <SddpConvergence results={results} />
            </TabsContent>
            <TabsContent value="merit">
              <MeritOrderTable results={results} />
            </TabsContent>
          </Tabs>
        </>
      )}
    </div>
  );
}
