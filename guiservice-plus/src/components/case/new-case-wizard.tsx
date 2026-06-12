'use client';

import * as React from 'react';
import * as DialogPrimitive from '@radix-ui/react-dialog';
import { useStore } from '@/lib/store';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { cn } from '@/lib/utils';
import type { CaseData } from '@/lib/api';

// ── template data ────────────────────────────────────────────────────────────

type SystemData = Record<string, Array<Record<string, unknown>>>;

const TEMPLATES: Record<
  string,
  { label: string; description: string; system: SystemData }
> = {
  blank: {
    label: 'Blank',
    description: '1 bus, 0 generators',
    system: { bus: [{ uid: 1, name: 'b1' }] },
  },
  'single-bus': {
    label: 'Single-bus dispatch',
    description: '1 bus, 1 thermal generator, 1 demand',
    system: {
      bus: [{ uid: 1, name: 'b1' }],
      generator: [{ uid: 1, name: 'g_thermal', bus: 'b1', pmax: 200, gcost: 30 }],
      demand: [{ uid: 1, name: 'd1', bus: 'b1', lmax: 150 }],
    },
  },
  '4bus': {
    label: '4-bus OPF',
    description: 'IEEE 4-bus: 4 buses, 2 generators, 2 demands, 5 lines',
    system: {
      bus: [
        { uid: 1, name: 'b1', reference_bus: true },
        { uid: 2, name: 'b2' },
        { uid: 3, name: 'b3' },
        { uid: 4, name: 'b4' },
      ],
      generator: [
        { uid: 1, name: 'g1', bus: 'b1', pmax: 300, gcost: 20 },
        { uid: 2, name: 'g2', bus: 'b2', pmax: 200, gcost: 35 },
      ],
      demand: [
        { uid: 1, name: 'd3', bus: 'b3', lmax: 150 },
        { uid: 2, name: 'd4', bus: 'b4', lmax: 100 },
      ],
      line: [
        { uid: 1, name: 'l1_2', bus_a: 'b1', bus_b: 'b2', tmax_ab: 250, tmax_ba: 250, reactance: 0.05 },
        { uid: 2, name: 'l1_3', bus_a: 'b1', bus_b: 'b3', tmax_ab: 250, tmax_ba: 250, reactance: 0.1 },
        { uid: 3, name: 'l2_3', bus_a: 'b2', bus_b: 'b3', tmax_ab: 150, tmax_ba: 150, reactance: 0.08 },
        { uid: 4, name: 'l2_4', bus_a: 'b2', bus_b: 'b4', tmax_ab: 100, tmax_ba: 100, reactance: 0.12 },
        { uid: 5, name: 'l3_4', bus_a: 'b3', bus_b: 'b4', tmax_ab: 100, tmax_ba: 100, reactance: 0.1 },
      ],
    },
  },
  '9bus': {
    label: '9-bus OPF',
    description: 'IEEE 9-bus: 9 buses, 3 generators, 3 demands, 9 lines',
    system: {
      bus: Array.from({ length: 9 }, (_, i) => ({
        uid: i + 1,
        name: `b${i + 1}`,
        ...(i === 0 ? { reference_bus: true } : {}),
      })),
      generator: [
        { uid: 1, name: 'g1', bus: 'b1', pmax: 250, gcost: 20 },
        { uid: 2, name: 'g2', bus: 'b2', pmax: 300, gcost: 35 },
        { uid: 3, name: 'g3', bus: 'b3', pmax: 270, gcost: 30 },
      ],
      demand: [
        { uid: 1, name: 'd5', bus: 'b5', lmax: 125 },
        { uid: 2, name: 'd6', bus: 'b6', lmax: 90 },
        { uid: 3, name: 'd8', bus: 'b8', lmax: 100 },
      ],
      line: [
        { uid: 1, name: 'l1_4', bus_a: 'b1', bus_b: 'b4', tmax_ab: 250, tmax_ba: 250, reactance: 0.0576 },
        { uid: 2, name: 'l4_5', bus_a: 'b4', bus_b: 'b5', tmax_ab: 250, tmax_ba: 250, reactance: 0.092 },
        { uid: 3, name: 'l5_6', bus_a: 'b5', bus_b: 'b6', tmax_ab: 150, tmax_ba: 150, reactance: 0.17 },
        { uid: 4, name: 'l3_6', bus_a: 'b3', bus_b: 'b6', tmax_ab: 300, tmax_ba: 300, reactance: 0.0586 },
        { uid: 5, name: 'l6_7', bus_a: 'b6', bus_b: 'b7', tmax_ab: 150, tmax_ba: 150, reactance: 0.1008 },
        { uid: 6, name: 'l7_8', bus_a: 'b7', bus_b: 'b8', tmax_ab: 250, tmax_ba: 250, reactance: 0.072 },
        { uid: 7, name: 'l8_2', bus_a: 'b8', bus_b: 'b2', tmax_ab: 250, tmax_ba: 250, reactance: 0.0625 },
        { uid: 8, name: 'l8_9', bus_a: 'b8', bus_b: 'b9', tmax_ab: 250, tmax_ba: 250, reactance: 0.161 },
        { uid: 9, name: 'l9_4', bus_a: 'b9', bus_b: 'b4', tmax_ab: 250, tmax_ba: 250, reactance: 0.085 },
      ],
    },
  },
  custom: {
    label: 'Custom',
    description: 'Start from scratch with no preset',
    system: {},
  },
};

const TEMPLATE_KEYS = Object.keys(TEMPLATES);

// ── helpers ───────────────────────────────────────────────────────────────────

function buildCaseData(opts: {
  template: string;
  nBlocks: number;
  blockDuration: number;
  nStages: number;
  nScenarios: number;
  scaleObjective: number;
  demandFailCost: number;
  useSingleBus: boolean;
  useKirchhoff: boolean;
  method: string;
}): CaseData {
  const tmpl = TEMPLATES[opts.template] ?? TEMPLATES['blank'];
  const blocksPerStage = Math.max(1, Math.floor(opts.nBlocks / opts.nStages));

  const blockArray = Array.from({ length: opts.nBlocks }, (_, i) => ({
    uid: i + 1,
    duration: opts.blockDuration,
  }));

  const stageArray = Array.from({ length: opts.nStages }, (_, i) => ({
    uid: i + 1,
    first_block: i * blocksPerStage,
    count_block:
      i === opts.nStages - 1
        ? opts.nBlocks - i * blocksPerStage
        : blocksPerStage,
    active: 1,
  }));

  const probFactor = 1 / opts.nScenarios;
  const scenarioArray = Array.from({ length: opts.nScenarios }, (_, i) => ({
    uid: i + 1,
    probability_factor: probFactor,
  }));

  return {
    case_name: `${opts.template}_case`,
    options: {
      use_single_bus: opts.useSingleBus,
      use_kirchhoff: opts.useKirchhoff,
      scale_objective: opts.scaleObjective,
      demand_fail_cost: opts.demandFailCost,
      method: opts.method,
      input_format: 'csv',
      output_format: 'csv',
    },
    simulation: { block_array: blockArray, stage_array: stageArray, scenario_array: scenarioArray },
    system: tmpl.system,
    data_files: {},
  };
}

// ── step labels ───────────────────────────────────────────────────────────────

const STEP_LABELS = ['Topology', 'Time horizon', 'Options'];

// ── wizard component ──────────────────────────────────────────────────────────

export function NewCaseWizard({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
}) {
  const resetCaseData = useStore((s) => s.resetCaseData);
  const updateCaseData = useStore((s) => s.updateCaseData);

  // reset step on close
  const [step, setStep] = React.useState(1);
  React.useEffect(() => {
    if (!open) setStep(1);
  }, [open]);

  // step 1
  const [template, setTemplate] = React.useState('blank');
  // step 2
  const [nBlocks, setNBlocks] = React.useState(1);
  const [blockDuration, setBlockDuration] = React.useState(1);
  const [nStages, setNStages] = React.useState(1);
  const [nScenarios, setNScenarios] = React.useState(1);
  // step 3
  const [scaleObjective, setScaleObjective] = React.useState(1000);
  const [demandFailCost, setDemandFailCost] = React.useState(1000);
  const [useSingleBus, setUseSingleBus] = React.useState(true);
  const [useKirchhoff, setUseKirchhoff] = React.useState(false);
  const [method, setMethod] = React.useState<'monolithic' | 'sddp'>(
    'monolithic',
  );

  const handleFinish = React.useCallback(() => {
    const caseData = buildCaseData({
      template,
      nBlocks,
      blockDuration,
      nStages,
      nScenarios,
      scaleObjective,
      demandFailCost,
      useSingleBus,
      useKirchhoff,
      method,
    });
    resetCaseData();
    updateCaseData(() => caseData);
    onOpenChange(false);
  }, [
    template, nBlocks, blockDuration, nStages, nScenarios,
    scaleObjective, demandFailCost, useSingleBus, useKirchhoff, method,
    resetCaseData, updateCaseData, onOpenChange,
  ]);

  return (
    <DialogPrimitive.Root open={open} onOpenChange={onOpenChange}>
      <DialogPrimitive.Portal>
        <DialogPrimitive.Overlay className="fixed inset-0 z-50 bg-black/50 backdrop-blur-sm data-[state=open]:animate-in data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=open]:fade-in-0" />
        <DialogPrimitive.Content className="fixed left-1/2 top-1/2 z-50 w-full max-w-xl -translate-x-1/2 -translate-y-1/2 rounded-xl border bg-background p-6 shadow-lg duration-200 data-[state=open]:animate-in data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=open]:fade-in-0 data-[state=closed]:zoom-out-95 data-[state=open]:zoom-in-95">
          <DialogPrimitive.Close className="absolute right-4 top-4 rounded-sm opacity-70 ring-offset-background transition-opacity hover:opacity-100 focus:outline-none focus:ring-2 focus:ring-ring focus:ring-offset-2">
            <span className="text-sm">✕</span>
            <span className="sr-only">Close</span>
          </DialogPrimitive.Close>

          {/* header */}
          <DialogPrimitive.Title className="text-lg font-semibold">
            New case wizard
          </DialogPrimitive.Title>
          <DialogPrimitive.Description className="mt-1 text-sm text-muted-foreground">
            Step {step} of {STEP_LABELS.length} — {STEP_LABELS[step - 1]}
          </DialogPrimitive.Description>

          {/* step indicator */}
          <div className="mt-4 flex gap-1">
            {STEP_LABELS.map((label, i) => (
              <div
                key={label}
                className={cn(
                  'h-1 flex-1 rounded-full transition-colors',
                  i + 1 <= step ? 'bg-primary' : 'bg-muted',
                )}
              />
            ))}
          </div>

          {/* step content */}
          <div className="mt-6 min-h-[260px]">
            {step === 1 && (
              <Step1
                template={template}
                setTemplate={setTemplate}
              />
            )}
            {step === 2 && (
              <Step2
                nBlocks={nBlocks}
                setNBlocks={setNBlocks}
                blockDuration={blockDuration}
                setBlockDuration={setBlockDuration}
                nStages={nStages}
                setNStages={setNStages}
                nScenarios={nScenarios}
                setNScenarios={setNScenarios}
              />
            )}
            {step === 3 && (
              <Step3
                scaleObjective={scaleObjective}
                setScaleObjective={setScaleObjective}
                demandFailCost={demandFailCost}
                setDemandFailCost={setDemandFailCost}
                useSingleBus={useSingleBus}
                setUseSingleBus={setUseSingleBus}
                useKirchhoff={useKirchhoff}
                setUseKirchhoff={setUseKirchhoff}
                method={method}
                setMethod={setMethod}
              />
            )}
          </div>

          {/* navigation */}
          <div className="mt-6 flex justify-between">
            <Button
              variant="outline"
              onClick={() => setStep((s) => s - 1)}
              disabled={step === 1}
            >
              Back
            </Button>
            {step < STEP_LABELS.length ? (
              <Button onClick={() => setStep((s) => s + 1)}>Next</Button>
            ) : (
              <Button onClick={handleFinish}>Finish</Button>
            )}
          </div>
        </DialogPrimitive.Content>
      </DialogPrimitive.Portal>
    </DialogPrimitive.Root>
  );
}

// ── sub-step components ───────────────────────────────────────────────────────

function Step1({
  template,
  setTemplate,
}: {
  template: string;
  setTemplate: (t: string) => void;
}) {
  return (
    <div className="space-y-2">
      <p className="text-sm font-medium">Choose a topology template:</p>
      <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
        {TEMPLATE_KEYS.map((key) => {
          const t = TEMPLATES[key];
          return (
            <label
              key={key}
              className={cn(
                'flex cursor-pointer items-start gap-3 rounded-lg border p-3 transition-colors hover:bg-accent',
                template === key && 'border-primary bg-accent',
              )}
            >
              <input
                type="radio"
                name="template"
                value={key}
                checked={template === key}
                onChange={() => setTemplate(key)}
                className="mt-0.5"
              />
              <div>
                <p className="text-sm font-medium">{t.label}</p>
                <p className="text-xs text-muted-foreground">{t.description}</p>
              </div>
            </label>
          );
        })}
      </div>
    </div>
  );
}

function Step2({
  nBlocks, setNBlocks,
  blockDuration, setBlockDuration,
  nStages, setNStages,
  nScenarios, setNScenarios,
}: {
  nBlocks: number; setNBlocks: (v: number) => void;
  blockDuration: number; setBlockDuration: (v: number) => void;
  nStages: number; setNStages: (v: number) => void;
  nScenarios: number; setNScenarios: (v: number) => void;
}) {
  return (
    <div className="space-y-4">
      <Button
        variant="outline"
        size="sm"
        onClick={() => { setNBlocks(1); setBlockDuration(1); setNStages(1); setNScenarios(1); }}
      >
        Quick preset: single snapshot
      </Button>
      <div className="grid grid-cols-2 gap-4">
        <div className="space-y-1">
          <Label htmlFor="nBlocks">Number of blocks (1–8760)</Label>
          <Input
            id="nBlocks"
            type="number"
            min={1}
            max={8760}
            value={nBlocks}
            onChange={(e) => setNBlocks(Math.max(1, Math.min(8760, parseInt(e.target.value) || 1)))}
          />
        </div>
        <div className="space-y-1">
          <Label htmlFor="blockDuration">Block duration (h)</Label>
          <Input
            id="blockDuration"
            type="number"
            min={0.01}
            step={0.5}
            value={blockDuration}
            onChange={(e) => setBlockDuration(parseFloat(e.target.value) || 1)}
          />
        </div>
        <div className="space-y-1">
          <Label htmlFor="nStages">Number of stages (1–20)</Label>
          <Input
            id="nStages"
            type="number"
            min={1}
            max={20}
            value={nStages}
            onChange={(e) => setNStages(Math.max(1, Math.min(20, parseInt(e.target.value) || 1)))}
          />
        </div>
        <div className="space-y-1">
          <Label htmlFor="nScenarios">Number of scenarios (1–10)</Label>
          <Input
            id="nScenarios"
            type="number"
            min={1}
            max={10}
            value={nScenarios}
            onChange={(e) => setNScenarios(Math.max(1, Math.min(10, parseInt(e.target.value) || 1)))}
          />
        </div>
      </div>
    </div>
  );
}

function Step3({
  scaleObjective, setScaleObjective,
  demandFailCost, setDemandFailCost,
  useSingleBus, setUseSingleBus,
  useKirchhoff, setUseKirchhoff,
  method, setMethod,
}: {
  scaleObjective: number; setScaleObjective: (v: number) => void;
  demandFailCost: number; setDemandFailCost: (v: number) => void;
  useSingleBus: boolean; setUseSingleBus: (v: boolean) => void;
  useKirchhoff: boolean; setUseKirchhoff: (v: boolean) => void;
  method: 'monolithic' | 'sddp'; setMethod: (v: 'monolithic' | 'sddp') => void;
}) {
  return (
    <div className="space-y-4">
      <div className="grid grid-cols-2 gap-4">
        <div className="space-y-1">
          <Label htmlFor="scaleObj">Scale objective</Label>
          <Input
            id="scaleObj"
            type="number"
            min={1}
            value={scaleObjective}
            onChange={(e) => setScaleObjective(parseFloat(e.target.value) || 1000)}
          />
        </div>
        <div className="space-y-1">
          <Label htmlFor="failCost">Demand fail cost ($/MWh)</Label>
          <Input
            id="failCost"
            type="number"
            min={0}
            value={demandFailCost}
            onChange={(e) => setDemandFailCost(parseFloat(e.target.value) || 1000)}
          />
        </div>
      </div>
      <div className="space-y-2">
        <label className="flex cursor-pointer items-center gap-2 text-sm">
          <input
            type="checkbox"
            checked={useSingleBus}
            onChange={(e) => setUseSingleBus(e.target.checked)}
          />
          Use single bus (copper plate)
        </label>
        <label className="flex cursor-pointer items-center gap-2 text-sm">
          <input
            type="checkbox"
            checked={useKirchhoff}
            onChange={(e) => setUseKirchhoff(e.target.checked)}
          />
          Use Kirchhoff (DC OPF)
        </label>
      </div>
      <div className="space-y-1">
        <Label htmlFor="method">Method</Label>
        <select
          id="method"
          className="w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
          value={method}
          onChange={(e) => setMethod(e.target.value as 'monolithic' | 'sddp')}
        >
          <option value="monolithic">Monolithic</option>
          <option value="sddp">SDDP</option>
        </select>
      </div>
    </div>
  );
}
