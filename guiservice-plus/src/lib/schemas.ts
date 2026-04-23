/**
 * Zod schemas mirroring the ELEMENT_SCHEMAS used by the Flask guiservice.
 * These provide per-field validation at the form layer before a case is
 * ever submitted to the server.
 *
 * The schemas intentionally use `passthrough()` on every object so that
 * unknown extension fields are preserved — the C++ solver ignores unknown
 * keys, and we want the GUI to be forward-compatible.
 */

import { z } from 'zod';

const numberOrFile = z.union([
  z.number(),
  z.string(),
  z.array(z.number()),
]);

const commonFields = {
  uid: z.number().int().nonnegative().optional(),
  name: z.string().min(1).optional(),
  active: z.union([z.number(), z.boolean(), z.array(z.number())]).optional(),
};

export const busSchema = z
  .object({
    ...commonFields,
    reference_bus: z.boolean().optional(),
    voltage: z.number().nonnegative().optional(),
  })
  .passthrough();

export const generatorSchema = z
  .object({
    ...commonFields,
    bus: z.union([z.string(), z.number()]),
    pmin: numberOrFile.optional(),
    pmax: numberOrFile.optional(),
    gcost: numberOrFile.optional(),
    capacity: z.number().optional(),
    expcap: z.number().optional(),
    expmod: z.number().optional(),
    annual_capcost: z.number().optional(),
  })
  .passthrough();

export const demandSchema = z
  .object({
    ...commonFields,
    bus: z.union([z.string(), z.number()]),
    lmax: numberOrFile.optional(),
    fail_cost: z.number().optional(),
  })
  .passthrough();

export const lineSchema = z
  .object({
    ...commonFields,
    bus_a: z.union([z.string(), z.number()]),
    bus_b: z.union([z.string(), z.number()]),
    tmax_ab: numberOrFile.optional(),
    tmax_ba: numberOrFile.optional(),
    reactance: z.number().nonnegative().optional(),
    resistance: z.number().nonnegative().optional(),
    tcost: z.number().optional(),
  })
  .passthrough();

export const batterySchema = z
  .object({
    ...commonFields,
    bus: z.union([z.string(), z.number()]).optional(),
    emax: z.number().nonnegative().optional(),
    emin: z.number().nonnegative().optional(),
    eini: z.number().nonnegative().optional(),
    efficiency_charge: z.number().min(0).max(1).optional(),
    efficiency_discharge: z.number().min(0).max(1).optional(),
  })
  .passthrough();

export const blockSchema = z
  .object({
    uid: z.number().int().nonnegative(),
    duration: z.number().positive(),
  })
  .passthrough();

export const stageSchema = z
  .object({
    uid: z.number().int().nonnegative(),
    first_block: z.number().int().nonnegative(),
    count_block: z.number().int().positive(),
    active: z.union([z.number(), z.boolean()]).optional(),
    discount_factor: z.number().positive().optional(),
  })
  .passthrough();

export const scenarioSchema = z
  .object({
    uid: z.number().int().nonnegative(),
    probability_factor: z.number().min(0),
    active: z.union([z.number(), z.boolean()]).optional(),
  })
  .passthrough();

export const optionsSchema = z
  .object({
    use_single_bus: z.boolean().optional(),
    use_kirchhoff: z.boolean().optional(),
    use_line_losses: z.boolean().optional(),
    scale_objective: z.number().positive().optional(),
    demand_fail_cost: z.number().nonnegative().optional(),
    reserve_fail_cost: z.number().nonnegative().optional(),
    input_directory: z.string().optional(),
    output_directory: z.string().optional(),
    input_format: z.enum(['parquet', 'csv']).optional(),
    output_format: z.enum(['parquet', 'csv']).optional(),
    output_compression: z.enum(['zstd', 'gzip', 'lzo', 'uncompressed']).optional(),
    method: z.enum(['monolithic', 'sddp', 'cascade']).optional(),
  })
  .passthrough();

export const ELEMENT_SCHEMAS = {
  bus: busSchema,
  generator: generatorSchema,
  demand: demandSchema,
  line: lineSchema,
  battery: batterySchema,
} as const;

export type ElementKind = keyof typeof ELEMENT_SCHEMAS;
