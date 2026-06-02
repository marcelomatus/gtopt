# rts_gmlc_to_gtopt

Tiny converter that ports the **RTS-GMLC (Reliability Test System — Grid
Modernization Lab Consortium)** benchmark into a gtopt planning JSON,
plus the integration test that exercises gtopt's emission framework with
**native per-generator multi-pollutant CO₂ data**.

## Source

* **Dataset**: GridMod consortium maintained at
  [github.com/GridMod/RTS-GMLC](https://github.com/GridMod/RTS-GMLC) —
  `RTS_Data/SourceData/` directory.
  Fetched on demand by `_converter.py` via `urllib`, cached under
  `~/.cache/gtopt/rts_gmlc/`.
* **Lineage**: RTS-GMLC is the 2019 Grid-Modernization update of the
  **IEEE Reliability Test System (RTS-96)** — the canonical
  power-system reliability benchmark since 1979. The GMLC update
  refreshed the generator fleet, added per-zone renewable profiles,
  per-generator reliability metrics (FOR, MTTF, MTTR), and — critically
  for this port — per-generator native emission rates for **7
  pollutants**.
* **Companion**: also used as the data source for the `pglib-uc`
  benchmark suite —
  [github.com/power-grid-lib/pglib-uc](https://github.com/power-grid-lib/pglib-uc).

## Why this port complements `nrel118_to_gtopt`

NREL-118 carries **per-fuel-family** CO₂ factors (one rate per fuel
type, mapped via IPCC AR6 defaults). RTS-GMLC carries
**per-generator** native rates baked into `gen.csv` for SEVEN
pollutants:

| Column | Pollutant | Unit |
|---|---|---|
| `Emissions CO2 Lbs/MMBTU` | CO₂ | Lbs / MMBtu |
| `Emissions SO2 Lbs/MMBTU` | SO₂ | Lbs / MMBtu |
| `Emissions NOX Lbs/MMBTU` | NOₓ | Lbs / MMBtu |
| `Emissions Part Lbs/MMBTU` | Particulates | Lbs / MMBtu |
| `Emissions CH4 Lbs/MMBTU` | CH₄ | Lbs / MMBtu |
| `Emissions N2O Lbs/MMBTU` | N₂O | Lbs / MMBtu |
| `Emissions CO Lbs/MMBTU` | CO | Lbs / MMBtu |
| `Emissions VOCs Lbs/MMBTU` | VOCs | Lbs / MMBtu |

This means **no IPCC overlay is needed** (and would be discouraged —
the per-gen rate already reflects unit-specific combustion technology /
emission-control retrofits). It also stresses gtopt's
multi-`emission_factors[]` path that the per-fuel-only ports don't
reach.

## What the converter does

1. **Download + cache** the canonical CSVs (`gen.csv`, `bus.csv`,
   `branch.csv`, plus annual hourly profiles) from
   `GridMod/RTS-GMLC/RTS_Data/SourceData/`, cached under
   `~/.cache/gtopt/rts_gmlc/`.
2. **Parse** the 158 generators across 73 buses with their
   piecewise heat-rate (`HR_avg_0` + `HR_incr_1..4` × `Output_pct_0..4`).
3. **Collapse to scalar HR** via pmax-weighted segment average
   (same approach as `nrel118_to_gtopt`, pending the multi-fuel feature
   in issue #510).
4. **Unit conversion** for CO₂: Lbs/MMBTU → tCO₂/GJ via
   `× 0.4536e-3 × (1/1.055) = × 0.0004299`.
   Sanity check on coal `101_STEAM_3`: ships 210 Lbs CO₂/MMBTU =
   0.0903 tCO₂/GJ (matches IPCC sub-bituminous coal ~0.0961 ± 5%).
5. **Emit per-gen `emission_factors[]`** with one row per pollutant
   present in the source CSV. Default emission_array carries CO₂; the
   `--emissions` master switch in the converter optionally adds
   NOₓ / SO₂ pollutants if downstream LP cares.

## CLI

```bash
PYTHONPATH=. python -m rts_gmlc_to_gtopt --day N -o OUT.json
```

`--day N` selects a 24-hour representative day from the annual series.

## What the integration test asserts

**File**: `test/source/test_emission_rts_gmlc_port.cpp`

Two TEST_CASEs:

1. **Per-generator emission-rate sanity** — for 5 named generators
   spanning fuel families (`101_STEAM_3` coal subcrit, `101_CT_1` oil,
   `118_CC_1` NG CC, `118_NUC_1` nuclear, `113_BIO_1` biomass), assert
   the synthesized `EmissionSource.rate` matches the analytic value
   `heat_rate [GJ/MWh] × combustion_factor [tCO₂/GJ]` computed
   directly from the per-gen CSV columns:

   ```
   rate_analytic = HR_btu_per_kwh × 1.055e-3       # BTU/kWh → GJ/MWh
                   × CO2_lbs_per_mmbtu × 0.4536e-3 # Lbs/MMBTU → tCO₂/MMBTU
                   / 1.055                          # MMBTU → GJ
                  = tCO₂/MWh
   ```

   Pre-computed expected rates:
   - Coal `101_STEAM_3`: 1.264 tCO₂/MWh (HR ≈ 11.5 GJ/MWh × 0.0903 from coal)
   - Oil `101_CT_1`: 0.952 tCO₂/MWh
   - NG CC `118_CC_1`: 0.379 tCO₂/MWh
   - Nuclear / biomass: ZERO synthesized rows (combustion factor = 0
     after IPCC biogenic-zero accounting).

2. **24-hour aggregate dispatch CO₂** — single-bus 24-hour LP, 600 MW
   load. The asserted bound is physical (`> 0`, `< 50 ktCO₂/day` for
   a ~10 GW peak system); RTS-GMLC has no single famous published
   reference number for total annual CO₂, so this assertion is loose
   on purpose. Day total ≈ 3.2 ktCO₂.

## Caveat: surprise behavior

`expand_fuel_emission_sources` in `source/system.cpp:602` **skips fuels
with `combustion == 0 && upstream == 0`** — so biomass and nuclear
generators get NO synthesized EmissionSource even when they declare a
fuel + heat_rate. Both tests account for this; per-gen sanity assertions
skip the nuclear/biomass entries.

## Related work

- Sister port `scripts/nrel118_to_gtopt/` covers per-fuel IPCC defaults
  on a different IEEE-class system; this port covers per-gen native
  multi-pollutant on the same scale.
- The `--emissions` flag and IPCC defaults are shared infrastructure
  in `gtopt_shared.emissions` and
  `gtopt_shared/data/ipcc_emission_factors.json`.
- Multi-pollutant LP coupling is exercised here for the first time
  (CO₂ + SO₂ + NOₓ) — useful baseline for any future EmissionZone
  basket implementation work.
