'use client';

import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import * as React from 'react';
import { optionsSchema } from '@/lib/schemas';
import { useStore } from '@/lib/store';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';

type OptionsForm = {
  use_single_bus?: boolean;
  use_kirchhoff?: boolean;
  use_line_losses?: boolean;
  scale_objective?: number;
  demand_fail_cost?: number;
  reserve_fail_cost?: number;
  input_directory?: string;
  output_directory?: string;
  input_format?: string;
  output_format?: string;
  output_compression?: string;
  method?: string;
};

export function OptionsForm() {
  const options = useStore((s) => s.caseData.options ?? {}) as OptionsForm;
  const updateCaseData = useStore((s) => s.updateCaseData);

  const { register, handleSubmit, formState } = useForm<OptionsForm>({
    resolver: zodResolver(optionsSchema),
    defaultValues: options,
  });

  const onBlur = handleSubmit((values) => {
    updateCaseData((draft) => ({
      ...draft,
      options: { ...(draft.options ?? {}), ...values },
    }));
  });

  return (
    <form className="grid grid-cols-1 gap-4 md:grid-cols-2" onBlur={onBlur}>
      <Field label="Scale objective" error={formState.errors.scale_objective?.message}>
        <Input
          type="number"
          step="any"
          {...register('scale_objective', { valueAsNumber: true })}
        />
      </Field>
      <Field label="Demand fail cost" error={formState.errors.demand_fail_cost?.message}>
        <Input
          type="number"
          step="any"
          {...register('demand_fail_cost', { valueAsNumber: true })}
        />
      </Field>
      <Field label="Input directory">
        <Input {...register('input_directory')} />
      </Field>
      <Field label="Output directory">
        <Input {...register('output_directory')} />
      </Field>
      <Field label="Input format">
        <select
          {...register('input_format')}
          className="h-10 rounded-md border border-input bg-background px-3 text-sm"
        >
          <option value="parquet">parquet</option>
          <option value="csv">csv</option>
        </select>
      </Field>
      <Field label="Output format">
        <select
          {...register('output_format')}
          className="h-10 rounded-md border border-input bg-background px-3 text-sm"
        >
          <option value="parquet">parquet</option>
          <option value="csv">csv</option>
        </select>
      </Field>
      <Field label="Method">
        <select
          {...register('method')}
          className="h-10 rounded-md border border-input bg-background px-3 text-sm"
        >
          <option value="monolithic">monolithic</option>
          <option value="sddp">sddp</option>
          <option value="cascade">cascade</option>
        </select>
      </Field>
      <Field label="Use single bus">
        <label className="flex h-10 items-center gap-2 text-sm">
          <input type="checkbox" {...register('use_single_bus')} /> Copper-plate
        </label>
      </Field>
    </form>
  );
}

function Field({
  label,
  error,
  children,
}: {
  label: string;
  error?: string;
  children: React.ReactNode;
}) {
  return (
    <div className="space-y-1">
      <Label>{label}</Label>
      {children}
      {error && <div className="text-xs text-destructive">{error}</div>}
    </div>
  );
}
