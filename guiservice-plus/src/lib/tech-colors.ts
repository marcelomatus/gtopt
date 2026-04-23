/**
 * Technology colour palette shared between charts and topology nodes.
 * Keys are lower-case technology labels used by the Python backend
 * (e.g. hydro, solar, wind, thermal, battery).
 */

export const TECH_COLORS: Record<string, string> = {
  hydro: 'hsl(204 82% 40%)',
  solar: 'hsl(38 92% 50%)',
  wind: 'hsl(142 71% 45%)',
  thermal: 'hsl(0 72% 51%)',
  battery: 'hsl(174 60% 42%)',
  reserve: 'hsl(262 83% 58%)',
  demand: 'hsl(24 95% 53%)',
  other: 'hsl(210 10% 50%)',
};

export const TECH_LABELS: Record<string, string> = {
  hydro: 'Hydro',
  solar: 'Solar',
  wind: 'Wind',
  thermal: 'Thermal',
  battery: 'Battery',
  reserve: 'Reserve',
  demand: 'Demand',
  other: 'Other',
};

export function techColor(tech: string | undefined | null): string {
  if (!tech) return TECH_COLORS.other;
  const key = tech.toLowerCase();
  return TECH_COLORS[key] ?? TECH_COLORS.other;
}

/**
 * Best-effort inference of a technology label from a generator name.
 * When the backend does not provide an explicit tech_map, the UI falls
 * back to name-based heuristics (e.g. "solar_01" → solar).
 */
export function inferTech(name: string | undefined | null): string {
  if (!name) return 'other';
  const n = name.toLowerCase();
  if (n.includes('solar') || n.includes('pv')) return 'solar';
  if (n.includes('wind') || n.includes('eolic')) return 'wind';
  if (n.includes('hydro') || n.startsWith('h_') || n.includes('hidro')) return 'hydro';
  if (n.includes('battery') || n.includes('bess') || n.includes('bat')) return 'battery';
  if (n.includes('therm') || n.includes('coal') || n.includes('gas') || n.includes('diesel')) {
    return 'thermal';
  }
  return 'other';
}
