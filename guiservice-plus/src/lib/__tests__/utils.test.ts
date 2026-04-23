import { describe, it, expect } from 'vitest';
import { formatNumber, safeJsonParse, cn } from '@/lib/utils';

describe('formatNumber', () => {
  it('formats plain numbers with 2 decimal places by default', () => {
    expect(formatNumber(1.5)).toBe('1.50');
    expect(formatNumber(0)).toBe('0.00');
    expect(formatNumber(-3.14)).toBe('-3.14');
  });

  it('uses k suffix for thousands', () => {
    expect(formatNumber(1500)).toBe('1.50 k');
    expect(formatNumber(999_999)).toBe('1000.00 k');
  });

  it('uses M suffix for millions', () => {
    expect(formatNumber(2_500_000)).toBe('2.50 M');
  });

  it('returns em-dash for null/undefined/NaN', () => {
    expect(formatNumber(null)).toBe('—');
    expect(formatNumber(undefined)).toBe('—');
    expect(formatNumber(NaN)).toBe('—');
  });

  it('returns infinity symbol for Infinity', () => {
    expect(formatNumber(Infinity)).toBe('∞');
    expect(formatNumber(-Infinity)).toBe('∞');
  });

  it('respects custom digits param', () => {
    expect(formatNumber(3.14159, 4)).toBe('3.1416');
    expect(formatNumber(1000, 0)).toBe('1 k');
  });
});

describe('safeJsonParse', () => {
  it('parses valid JSON', () => {
    expect(safeJsonParse('{"a":1}', {})).toEqual({ a: 1 });
  });

  it('returns fallback on invalid JSON', () => {
    expect(safeJsonParse('not json', 42)).toBe(42);
    expect(safeJsonParse('', [])).toEqual([]);
    expect(safeJsonParse('{broken', null)).toBeNull();
  });
});

describe('cn', () => {
  it('merges class names', () => {
    const result = cn('a', 'b');
    expect(result).toContain('a');
    expect(result).toContain('b');
  });

  it('handles conditional classes', () => {
    const result = cn('base', false && 'never');
    expect(result).toBe('base');
  });

  it('deduplicates tailwind conflicting classes', () => {
    // tailwind-merge should keep only the last padding
    const result = cn('px-2', 'px-4');
    expect(result).toBe('px-4');
  });
});
