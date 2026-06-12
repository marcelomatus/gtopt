'use client';

import * as React from 'react';
import { useQuery } from '@tanstack/react-query';
import ReactFlow, {
  Background,
  Controls,
  MiniMap,
  type Node,
  type Edge,
} from 'reactflow';
import 'reactflow/dist/style.css';
import { api } from '@/lib/api';
import { useStore } from '@/lib/store';
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import { techColor } from '@/lib/tech-colors';

function nodeStyle(kind: string) {
  const color = (() => {
    switch (kind) {
      case 'bus':
        return 'hsl(217 91% 45%)';
      case 'generator':
        return 'hsl(0 72% 51%)';
      case 'demand':
        return 'hsl(24 95% 53%)';
      case 'battery':
        return 'hsl(174 60% 42%)';
      case 'junction':
        return 'hsl(204 82% 40%)';
      case 'reservoir':
        return 'hsl(204 82% 32%)';
      default:
        return techColor(kind);
    }
  })();
  return {
    background: color,
    color: 'white',
    border: '1px solid rgba(0,0,0,0.1)',
    borderRadius: 8,
    padding: '6px 10px',
    fontSize: 12,
    fontWeight: 500,
  } as React.CSSProperties;
}

export default function TopologyPage() {
  const caseData = useStore((s) => s.caseData);

  const topology = useQuery({
    queryKey: ['topology', caseData],
    queryFn: () => api.topologyReactflow(caseData),
    enabled: (caseData.system?.bus ?? []).length > 0,
  });

  const { nodes, edges } = React.useMemo<{ nodes: Node[]; edges: Edge[] }>(() => {
    if (!topology.data) return { nodes: [], edges: [] };
    const ns: Node[] = topology.data.nodes.map((n) => ({
      id: n.id,
      position: n.position,
      data: { label: `${(n.data?.label as string) ?? n.id}` },
      style: nodeStyle((n.data?.kind as string) ?? n.type),
    }));
    const es: Edge[] = topology.data.edges.map((e) => ({
      id: e.id,
      source: e.source,
      target: e.target,
      label: e.label,
      animated: e.animated,
      style: e.style,
    }));
    return { nodes: ns, edges: es };
  }, [topology.data]);

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-semibold tracking-tight">Topology</h1>
          <p className="text-muted-foreground">
            Interactive view of the current case. Buses, generators, demands,
            lines, batteries, junctions, and reservoirs.
          </p>
        </div>
      </div>

      <Card className="h-[calc(100vh-220px)] overflow-hidden">
        <CardHeader>
          <CardTitle>Network graph</CardTitle>
          <CardDescription>
            {topology.isError
              ? 'Failed to load topology — is the Flask guiservice reachable?'
              : topology.isLoading
                ? 'Loading…'
                : `${topology.data?.meta?.n_nodes ?? 0} nodes • ${topology.data?.meta?.n_edges ?? 0} edges`}
          </CardDescription>
        </CardHeader>
        <CardContent className="h-full p-0">
          {nodes.length === 0 ? (
            <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
              {topology.isLoading ? 'Loading…' : 'No topology yet. Add buses and lines in the case editor.'}
            </div>
          ) : (
            <ReactFlow nodes={nodes} edges={edges} fitView>
              <Background gap={18} />
              <MiniMap />
              <Controls />
            </ReactFlow>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
