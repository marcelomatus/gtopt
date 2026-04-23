import { describe, it, expect } from 'vitest';
import {
  busSchema,
  generatorSchema,
  demandSchema,
  lineSchema,
  batterySchema,
  blockSchema,
  stageSchema,
} from '@/lib/schemas';

describe('busSchema', () => {
  it('accepts a minimal bus', () => {
    const result = busSchema.safeParse({ uid: 1, name: 'b1' });
    expect(result.success).toBe(true);
  });

  it('accepts a bus with optional fields', () => {
    const result = busSchema.safeParse({
      uid: 2,
      name: 'b2',
      reference_bus: true,
      voltage: 1.0,
    });
    expect(result.success).toBe(true);
  });

  it('rejects negative voltage', () => {
    const result = busSchema.safeParse({ uid: 1, name: 'b1', voltage: -1 });
    expect(result.success).toBe(false);
  });

  it('preserves unknown fields via passthrough', () => {
    const result = busSchema.safeParse({ uid: 1, name: 'b1', extra: 'data' });
    expect(result.success).toBe(true);
    expect((result.data as Record<string, unknown>).extra).toBe('data');
  });
});

describe('generatorSchema', () => {
  it('requires bus field', () => {
    const result = generatorSchema.safeParse({ uid: 1, name: 'g1' });
    expect(result.success).toBe(false);
  });

  it('accepts bus as string or number', () => {
    expect(generatorSchema.safeParse({ uid: 1, name: 'g1', bus: 'b1' }).success).toBe(true);
    expect(generatorSchema.safeParse({ uid: 1, name: 'g1', bus: 1 }).success).toBe(true);
  });

  it('accepts pmax as number, string, or array', () => {
    expect(
      generatorSchema.safeParse({ uid: 1, name: 'g1', bus: 'b1', pmax: 100 }).success,
    ).toBe(true);
    expect(
      generatorSchema.safeParse({
        uid: 1,
        name: 'g1',
        bus: 'b1',
        pmax: 'pmax.parquet',
      }).success,
    ).toBe(true);
    expect(
      generatorSchema.safeParse({ uid: 1, name: 'g1', bus: 'b1', pmax: [100, 200] }).success,
    ).toBe(true);
  });
});

describe('batterySchema', () => {
  it('rejects efficiency_charge > 1', () => {
    const result = batterySchema.safeParse({
      uid: 1,
      name: 'bat1',
      efficiency_charge: 1.5,
    });
    expect(result.success).toBe(false);
  });

  it('rejects negative emax', () => {
    const result = batterySchema.safeParse({ uid: 1, name: 'bat1', emax: -100 });
    expect(result.success).toBe(false);
  });

  it('accepts valid battery', () => {
    const result = batterySchema.safeParse({
      uid: 1,
      name: 'bat1',
      emax: 1000,
      emin: 0,
      efficiency_charge: 0.95,
      efficiency_discharge: 0.95,
    });
    expect(result.success).toBe(true);
  });
});

describe('lineSchema', () => {
  it('requires bus_a and bus_b', () => {
    const result = lineSchema.safeParse({ uid: 1, name: 'l1' });
    expect(result.success).toBe(false);
  });

  it('rejects negative reactance', () => {
    const result = lineSchema.safeParse({
      uid: 1,
      name: 'l1',
      bus_a: 'b1',
      bus_b: 'b2',
      reactance: -0.1,
    });
    expect(result.success).toBe(false);
  });
});

describe('blockSchema', () => {
  it('requires positive duration', () => {
    const bad = blockSchema.safeParse({ uid: 1, duration: 0 });
    expect(bad.success).toBe(false);
    const ok = blockSchema.safeParse({ uid: 1, duration: 1 });
    expect(ok.success).toBe(true);
  });
});

describe('stageSchema', () => {
  it('requires positive count_block', () => {
    const bad = stageSchema.safeParse({ uid: 1, first_block: 0, count_block: 0 });
    expect(bad.success).toBe(false);
    const ok = stageSchema.safeParse({ uid: 1, first_block: 0, count_block: 1 });
    expect(ok.success).toBe(true);
  });
});
