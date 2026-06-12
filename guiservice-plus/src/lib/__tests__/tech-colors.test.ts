import { describe, it, expect } from 'vitest';
import { techColor, inferTech, TECH_COLORS, TECH_LABELS } from '@/lib/tech-colors';

describe('techColor', () => {
  it('returns the correct colour for known techs', () => {
    expect(techColor('hydro')).toBe(TECH_COLORS.hydro);
    expect(techColor('solar')).toBe(TECH_COLORS.solar);
    expect(techColor('wind')).toBe(TECH_COLORS.wind);
    expect(techColor('thermal')).toBe(TECH_COLORS.thermal);
    expect(techColor('battery')).toBe(TECH_COLORS.battery);
  });

  it('is case-insensitive', () => {
    expect(techColor('HYDRO')).toBe(TECH_COLORS.hydro);
    expect(techColor('Solar')).toBe(TECH_COLORS.solar);
  });

  it('returns other colour for unknown tech', () => {
    expect(techColor('nuclear')).toBe(TECH_COLORS.other);
    expect(techColor('unknown')).toBe(TECH_COLORS.other);
  });

  it('returns other colour for null/undefined', () => {
    expect(techColor(null)).toBe(TECH_COLORS.other);
    expect(techColor(undefined)).toBe(TECH_COLORS.other);
  });
});

describe('inferTech', () => {
  it('infers solar from name patterns', () => {
    expect(inferTech('solar_farm_1')).toBe('solar');
    expect(inferTech('PV_plant')).toBe('solar');
    expect(inferTech('SOLAR')).toBe('solar');
  });

  it('infers wind from name patterns', () => {
    expect(inferTech('wind_turbine')).toBe('wind');
    expect(inferTech('eolic_unit_3')).toBe('wind');
  });

  it('infers hydro from name patterns', () => {
    expect(inferTech('h_run_of_river')).toBe('hydro');
    expect(inferTech('hidro_central')).toBe('hydro');
    expect(inferTech('hydro_dam')).toBe('hydro');
  });

  it('infers battery from name patterns', () => {
    expect(inferTech('bess_unit')).toBe('battery');
    expect(inferTech('bat_storage')).toBe('battery');
    expect(inferTech('Battery1')).toBe('battery');
  });

  it('infers thermal from name patterns', () => {
    expect(inferTech('gas_ccgt')).toBe('thermal');
    expect(inferTech('coal_plant')).toBe('thermal');
    expect(inferTech('diesel_gen')).toBe('thermal');
    expect(inferTech('thermal_unit')).toBe('thermal');
  });

  it('returns other for unrecognised names', () => {
    expect(inferTech('unit_001')).toBe('other');
    expect(inferTech(null)).toBe('other');
    expect(inferTech(undefined)).toBe('other');
  });
});

describe('TECH_LABELS', () => {
  it('has human-readable labels for all tech keys', () => {
    expect(TECH_LABELS.hydro).toBe('Hydro');
    expect(TECH_LABELS.solar).toBe('Solar');
    expect(TECH_LABELS.wind).toBe('Wind');
    expect(TECH_LABELS.thermal).toBe('Thermal');
    expect(TECH_LABELS.battery).toBe('Battery');
  });
});
