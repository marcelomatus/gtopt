import { clsx, type ClassValue } from 'clsx';
import { twMerge } from 'tailwind-merge';

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

export function formatNumber(value: number | null | undefined, digits = 2): string {
  if (value === null || value === undefined || Number.isNaN(value)) {
    return '—';
  }
  if (!Number.isFinite(value)) {
    return '∞';
  }
  const absV = Math.abs(value);
  if (absV >= 1_000_000) return `${(value / 1_000_000).toFixed(digits)} M`;
  if (absV >= 1_000) return `${(value / 1_000).toFixed(digits)} k`;
  return value.toFixed(digits);
}

export function safeJsonParse<T>(raw: string, fallback: T): T {
  try {
    return JSON.parse(raw) as T;
  } catch {
    return fallback;
  }
}
