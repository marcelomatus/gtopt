'use client';
/**
 * Custom ReactFlow node types for the gtopt topology canvas.
 */

import * as React from 'react';
import {
  Handle,
  Position,
  BaseEdge,
  EdgeLabelRenderer,
  getBezierPath,
  type NodeProps,
  type EdgeProps,
} from 'reactflow';
import { Zap, TrendingDown, Battery, Circle } from 'lucide-react';
import { Badge } from '@/components/ui/badge';
import { techColor, inferTech } from '@/lib/tech-colors';

export function BusNode({ data }: NodeProps) {
  const label = (data?.label as string) ?? (data?.name as string) ?? 'Bus';
  const lmp = data?.lmp as number | undefined;

  return (
    <div
      className="flex flex-col items-center justify-center rounded-full border-2 border-blue-500 bg-blue-100 dark:bg-blue-900"
      style={{ width: 72, height: 72 }}
    >
      <Handle type="target" position={Position.Top} />
      <Handle type="source" position={Position.Bottom} />
      <Circle className="mb-0.5 h-4 w-4 text-blue-600" />
      <span className="max-w-[64px] truncate text-center text-[10px] font-semibold leading-tight text-blue-800 dark:text-blue-200">
        {label}
      </span>
      {lmp !== undefined && (
        <Badge
          variant="secondary"
          className="mt-0.5 px-1 py-0 text-[9px] leading-tight"
        >
          {lmp.toFixed(1)} $/MWh
        </Badge>
      )}
    </div>
  );
}

export function GeneratorNode({ data }: NodeProps) {
  const name = (data?.label as string) ?? (data?.name as string) ?? 'Gen';
  const pmax = data?.pmax as number | undefined;
  const gcost = data?.gcost as number | undefined;
  const tech = inferTech(name);
  const strip = techColor(tech);

  return (
    <div className="relative min-w-[110px] overflow-hidden rounded-lg border border-border bg-card shadow-sm">
      <Handle type="target" position={Position.Left} />
      <Handle type="source" position={Position.Right} />
      <div className="h-1.5 w-full" style={{ background: strip }} />
      <div className="px-2 py-1.5">
        <div className="flex items-center gap-1">
          <Zap className="h-3 w-3 text-yellow-500" />
          <span className="max-w-[90px] truncate text-[11px] font-semibold">{name}</span>
        </div>
        {pmax !== undefined && (
          <div className="text-[10px] text-muted-foreground">{pmax} MW</div>
        )}
        {gcost !== undefined && (
          <div className="text-[10px] text-muted-foreground">{gcost} $/MWh</div>
        )}
      </div>
    </div>
  );
}

export function DemandNode({ data }: NodeProps) {
  const name = (data?.label as string) ?? (data?.name as string) ?? 'Demand';
  const lmax = data?.lmax as number | undefined;

  return (
    <div className="min-w-[100px] overflow-hidden rounded-lg border border-orange-300 bg-orange-50 shadow-sm dark:bg-orange-950">
      <Handle type="target" position={Position.Top} />
      <Handle type="source" position={Position.Bottom} />
      <div className="h-1.5 w-full bg-orange-400" />
      <div className="px-2 py-1.5">
        <div className="flex items-center gap-1">
          <TrendingDown className="h-3 w-3 text-orange-500" />
          <span className="max-w-[88px] truncate text-[11px] font-semibold text-orange-800 dark:text-orange-200">
            {name}
          </span>
        </div>
        {lmax !== undefined && (
          <div className="text-[10px] text-orange-600 dark:text-orange-400">{lmax} MW</div>
        )}
      </div>
    </div>
  );
}

export function BatteryNode({ data }: NodeProps) {
  const name = (data?.label as string) ?? (data?.name as string) ?? 'Battery';
  const emax = data?.emax as number | undefined;
  const eff = data?.efficiency_charge as number | undefined;

  return (
    <div className="min-w-[110px] overflow-hidden rounded-lg border border-teal-300 bg-teal-50 shadow-sm dark:bg-teal-950">
      <Handle type="target" position={Position.Left} />
      <Handle type="source" position={Position.Right} />
      <div className="h-1.5 w-full bg-teal-500" />
      <div className="px-2 py-1.5">
        <div className="flex items-center gap-1">
          <Battery className="h-3 w-3 text-teal-600" />
          <span className="max-w-[88px] truncate text-[11px] font-semibold text-teal-800 dark:text-teal-200">
            {name}
          </span>
        </div>
        {emax !== undefined && (
          <div className="text-[10px] text-teal-600 dark:text-teal-400">{emax} MWh</div>
        )}
        {eff !== undefined && (
          <div className="text-[10px] text-teal-600 dark:text-teal-400">η={eff}</div>
        )}
      </div>
    </div>
  );
}

export function LineEdge({
  id,
  sourceX,
  sourceY,
  targetX,
  targetY,
  sourcePosition,
  targetPosition,
  data,
  markerEnd,
  style,
}: EdgeProps) {
  const [edgePath, labelX, labelY] = getBezierPath({
    sourceX,
    sourceY,
    sourcePosition,
    targetX,
    targetY,
    targetPosition,
  });

  const tmax = data?.tmax_ab as number | undefined;
  const flow = data?.flow as number | undefined;
  const nearLimit =
    tmax !== undefined && flow !== undefined && Math.abs(flow) / tmax > 0.85;
  const edgeColor = nearLimit ? '#ef4444' : (style?.stroke as string) ?? '#94a3b8';

  return (
    <>
      <BaseEdge
        id={id}
        path={edgePath}
        markerEnd={markerEnd}
        style={{ ...style, stroke: edgeColor, strokeWidth: nearLimit ? 2.5 : 1.5 }}
      />
      {tmax !== undefined && (
        <EdgeLabelRenderer>
          <div
            style={{
              position: 'absolute',
              transform: `translate(-50%, -50%) translate(${labelX}px,${labelY}px)`,
              pointerEvents: 'all',
            }}
            className="nodrag nopan rounded bg-white/90 px-1 py-0.5 text-[9px] font-medium shadow dark:bg-gray-800/90"
          >
            {tmax} MW
          </div>
        </EdgeLabelRenderer>
      )}
    </>
  );
}

export const customNodeTypes = {
  bus: BusNode,
  generator: GeneratorNode,
  demand: DemandNode,
  battery: BatteryNode,
};

export const customEdgeTypes = {
  line: LineEdge,
};
