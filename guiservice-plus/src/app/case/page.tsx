'use client';

import * as React from 'react';
import { useQuery } from '@tanstack/react-query';
import { useSearchParams } from 'next/navigation';
import { api, type ValidationResult } from '@/lib/api';
import { useStore } from '@/lib/store';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import {
  Tabs,
  TabsList,
  TabsTrigger,
  TabsContent,
} from '@/components/ui/tabs';
import { OptionsForm } from '@/components/case/options-form';
import { SimulationEditor } from '@/components/case/simulation-editor';
import { ElementEditor } from '@/components/case/element-editor';
import { CaseHealthPanel } from '@/components/case/case-health-panel';
import { JsonPreviewDialog } from '@/components/case/json-preview-dialog';
import { ElementKind } from '@/lib/schemas';

const SECTIONS: ElementKind[] = ['bus', 'generator', 'demand', 'line', 'battery'];

export default function CaseEditorPage() {
  const search = useSearchParams();
  const templateSlug = search.get('template');
  const caseData = useStore((s) => s.caseData);
  const setCaseData = useStore((s) => s.setCaseData);

  // Load a template into the store when ?template= is present.
  React.useEffect(() => {
    if (!templateSlug) return;
    let cancelled = false;
    api
      .getTemplate(templateSlug)
      .then((data) => {
        if (!cancelled) setCaseData(data);
      })
      .catch(() => {
        /* ignore — show user-facing error via toast in a future iteration */
      });
    return () => {
      cancelled = true;
    };
  }, [templateSlug, setCaseData]);

  const validation = useQuery<ValidationResult>({
    queryKey: ['validate', caseData],
    queryFn: () => api.validateCase(caseData),
    // Only run when caseData has at least some content
    enabled: Object.keys(caseData.system ?? {}).length > 0,
  });

  const [jsonOpen, setJsonOpen] = React.useState(false);

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-semibold tracking-tight">
            Case editor
          </h1>
          <p className="text-muted-foreground">
            {caseData.case_name ?? '(unnamed case)'}
          </p>
        </div>
        <div className="flex gap-2">
          <Button variant="outline" onClick={() => setJsonOpen(true)}>
            Preview JSON
          </Button>
        </div>
      </div>

      <div className="grid grid-cols-1 gap-6 lg:grid-cols-4">
        <div className="space-y-4 lg:col-span-3">
          <Tabs defaultValue="options">
            <TabsList>
              <TabsTrigger value="options">Options</TabsTrigger>
              <TabsTrigger value="simulation">Simulation</TabsTrigger>
              {SECTIONS.map((s) => (
                <TabsTrigger key={s} value={s}>
                  {s} {(() => {
                    const items = (caseData.system?.[s] ?? []) as unknown[];
                    return items.length > 0 ? (
                      <Badge variant="outline" className="ml-2">
                        {items.length}
                      </Badge>
                    ) : null;
                  })()}
                </TabsTrigger>
              ))}
            </TabsList>

            <TabsContent value="options">
              <Card>
                <CardHeader>
                  <CardTitle>Options</CardTitle>
                  <CardDescription>
                    Global solver and I/O options.
                  </CardDescription>
                </CardHeader>
                <CardContent>
                  <OptionsForm />
                </CardContent>
              </Card>
            </TabsContent>

            <TabsContent value="simulation">
              <Card>
                <CardHeader>
                  <CardTitle>Simulation</CardTitle>
                  <CardDescription>
                    Blocks, stages, and scenarios.
                  </CardDescription>
                </CardHeader>
                <CardContent>
                  <SimulationEditor />
                </CardContent>
              </Card>
            </TabsContent>

            {SECTIONS.map((kind) => (
              <TabsContent key={kind} value={kind}>
                <Card>
                  <CardHeader>
                    <CardTitle className="capitalize">{kind}</CardTitle>
                    <CardDescription>
                      Add, edit, or remove {kind} elements.
                    </CardDescription>
                  </CardHeader>
                  <CardContent>
                    <ElementEditor kind={kind} />
                  </CardContent>
                </Card>
              </TabsContent>
            ))}
          </Tabs>
        </div>

        <div className="space-y-4 lg:col-span-1">
          <CaseHealthPanel validation={validation.data} loading={validation.isLoading} />
        </div>
      </div>

      <JsonPreviewDialog open={jsonOpen} onOpenChange={setJsonOpen} />
    </div>
  );
}
