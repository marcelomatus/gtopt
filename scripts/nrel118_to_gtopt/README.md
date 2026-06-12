# nrel118_to_gtopt

Tiny converter that ports the **NREL-118 extended IEEE 118-bus test system**
into a gtopt planning JSON, plus the integration test that exercises gtopt's
emission framework end-to-end against this benchmark.

## Source

The benchmark is the publicly published NREL extension of the IEEE 118-bus
test system, designed for studies of high-renewable-penetration scenarios:

* **Paper**: Peña, I.; Martinez-Anido, C. B.; Hodge, B.-M. *An Extended
  IEEE 118-Bus Test System With High Renewable Penetration.*
  IEEE Transactions on Power Systems, Vol. 33, No. 1, January 2018,
  pp. 281-289.
  DOI: [10.1109/TPWRS.2017.2695963](https://doi.org/10.1109/TPWRS.2017.2695963)
* **Dataset**: `NREL-Sienna/PowerSystemsTestData` repository, `118-Bus/`
  directory —
  [github.com/NREL-Sienna/PowerSystemsTestData/tree/master/118-Bus](https://github.com/NREL-Sienna/PowerSystemsTestData/tree/master/118-Bus).
  Fetched on demand by `_converter.py` via `urllib`, cached under
  `~/.cache/gtopt/nrel118/`.

The dataset ships **10 generation technologies** (coal subcritical, coal
supercritical, natural-gas CC, natural-gas CT, oil CT, biomass, hydro,
wind, solar, nuclear) with piecewise heat-rate functions (Heat Rate Base
MMBTU/hr + 5 incremental bands BTU/kWh), minimum stable levels, ramping
rates, start costs, and 8784-hour time-synchronous load / wind / solar
series.

## What the converter does

1. **Download + cache** the four canonical CSVs (`Generators.csv`,
   `Buses.csv`, `Lines.csv`, plus per-zone hourly profiles) from
   NREL-Sienna/PowerSystemsTestData on first run, cached under
   `~/.cache/gtopt/nrel118/`.
2. **Parse** the per-generator piecewise heat-rate data
   (Heat Rate Base + Inc Band 1-5 over Load Point Band 1-5).
3. **Collapse to scalar HR** via pmax-weighted segment average so the
   per-generator heat_rate is in GJ/MWh (canonical for gtopt's emission
   pipeline today — gtopt's `expand_fuel_emission_sources` requires
   scalar HR and skips piecewise with a WARN, pending the multi-fuel
   feature tracked in issue #510).
4. **Assign per-fuel CO₂ factors** from IPCC AR6 Table A.III.2 in a
   small inline table (NO modification to the shared
   `gtopt_shared/data/ipcc_emission_factors.json` — the per-fuel mapping
   for this specific study is study-specific, not a global default):

   | Fuel family | tCO₂/GJ | Source |
   |---|---|---|
   | Coal (subcritical, supercritical) | 0.0946 | IPCC AR6 WG3 Table A.III.2 |
   | Natural Gas (CC, CT) | 0.0561 | IPCC AR6 WG3 Table A.III.2 |
   | Oil / Diesel | 0.0741 | IPCC AR6 WG3 Table A.III.2 |
   | Biomass | 0.0 | Biogenic-zero per IPCC AFOLU |
   | Nuclear / Wind / Solar / Hydro | 0.0 | No fuel combustion |

5. **Emit gtopt JSON** with `Fuel.heat_content` unset (HR is already in
   GJ/MWh), so gtopt's `heat_content` multiplication path doesn't fire
   for this case — the formula collapses to the legacy `rate = hr ×
   combustion`, exercised in the C++ test alongside the multiplication
   path tested by the CEN-Chile bundles.

## CLI

```bash
PYTHONPATH=. python -m nrel118_to_gtopt --week N -o OUT.json
```

`--week N` selects a single representative week from the 8784-hour annual
series (default = winter peak, around week 2 of January). The single-week
horizon keeps the LP small enough for a CI integration test.

## What the integration test asserts

**File**: `test/source/test_emission_nrel118_port.cpp`

Two TEST_CASEs:

1. **Baseline thermal dispatch** — 6-thermal subset of the NREL-118
   fleet (one per fuel-family rank: nuclear, coal subcrit, CC NG,
   biomass, CT NG, CT Oil), no renewables, 1100 MW demand on a single
   bus. Merit-order dispatch is solved; CO₂ total ≈ 368 t for the
   single hour. Asserts the per-MWh rates synthesized by
   `expand_fuel_emission_sources` match `heat_rate × combustion`
   to numerical tolerance.

2. **33%-renewables scenario vs baseline** — Same fleet plus a 363 MW
   renewable injection (1100 × 0.33). Thermals derate proportionally.
   CO₂ drops ~33% versus baseline. The assertion window is
   **[15%, 50%]** — wider than the paper's annual 29-34% band because
   a single representative hour is more variable than the full-year
   average. The published reference number is the single regression
   target this whole port chases.

### Regression target (the gold)

> *"At 33% wind+solar penetration, the system avoids 29-34% of CO₂
> emissions, 16-22% of NOₓ, and 14-24% of SO₂ vs. the baseline thermal
> mix."* — Peña et al. 2018, abstract.

The integration test windows this delta to [15%, 50%] for a single-hour
fixture (the paper's number is an 8784-hour average). Tightening the
window requires solving the full annual horizon — out of scope for CI.

## Caveat: surprise behavior

`expand_fuel_emission_sources` in `source/system.cpp:602` **skips fuels
with `combustion == 0 && upstream == 0`** — so biomass and nuclear
generators get NO synthesized EmissionSource even when they declare a
fuel + heat_rate. This is correct (zero-rate sources would be pure LP
overhead) but means the per-EmissionSource count is fewer than the
per-fueled-generator count. Both tests in this port account for this.

## Related work

- The `--emissions` flag and IPCC defaults are shared infrastructure
  living in `gtopt_shared.emissions` and
  `gtopt_shared/data/ipcc_emission_factors.json`.
- The piecewise→scalar fallback (this port's `collapse_piecewise_hr`)
  is a converter-side workaround pending gtopt-side multi-fuel /
  per-fuel HR support tracked in
  [issue #510](https://github.com/marcelomatus/gtopt/issues/510).
- Sister ports for cross-validation:
  `scripts/rts_gmlc_to_gtopt/` (GridMod RTS-GMLC, per-gen native CO₂
  multi-pollutant), `scripts/sienna_to_gtopt/` (5-bus non-emission
  variants for cascading hydro / monitored line / HVDC).
