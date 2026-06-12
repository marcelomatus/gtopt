# sddp2gtopt — Design Doc (v0)

Convert a **PSR SDDP** case directory to a gtopt JSON planning, mirroring the
role `plp2gtopt` plays for PLP cases. Same CLI ergonomics, same
JSON+Parquet output, same `gtopt_writer` integration; the only thing
that changes is the input dialect.

## 1. Scope

- **In:** PSR SDDP cases (Brazilian/PSR Inc. format — `.dat` files plus
  `psrclasses.json`, as shipped by `PSRClassesInterface.jl/test/data/case0..5`
  and PSR SDDP installations).
- **Out (explicitly):** SDDP.jl (Oscar Dowson) `.sof.json` cases — different
  format, academic, not what the users mean.
- **First milestone:** read `case0` end-to-end and produce a gtopt JSON that
  `gtopt --lp-only` accepts (matches the `plp2gtopt-run` skill contract).

## 2. Two-tier reader strategy

`psrclasses.json` is a flattened, typed snapshot of all `.dat` files written
by SDDP itself when a study is saved. v0 reads JSON only; v1 falls back to
raw `.dat` parsers when JSON is absent.

| Tier | Input                                | Effort  | Coverage          |
|------|--------------------------------------|---------|-------------------|
| v0   | `psrclasses.json` only               | small   | modern SDDP cases |
| v1   | + `.dat` parsers (one per file kind) | medium  | legacy cases      |
| v2   | + `.pmd` schema validation           | small   | strict mode       |

This is the **inverse** of plp2gtopt's evolution (which had to start from
Fortran-binary `.dat` files because PLP has no JSON manifest). Starting
from JSON makes v0 essentially a schema mapper — a few hundred lines.

## 3. Layout (mirrors `scripts/plp2gtopt/`)

```
scripts/sddp2gtopt/
├── __init__.py
├── main.py                # argparse entry, --info / --validate / convert
├── sddp2gtopt.py          # convert_sddp_case, validate_sddp_case
├── base_parser.py         # shared base (likely just re-export plp2gtopt's)
├── sddp_parser.py         # orchestrator, reads psrclasses.json
├── system_parser.py       # PSRSystem
├── bus_parser.py          # PSRBus / PSRArea
├── circuit_parser.py      # PSRCircuito / PSRTransformador → Lines
├── interconnection_parser.py  # PSRInterconnection (area-level link)
├── hydro_parser.py        # PSRHydroPlant, PSRGaugingStation, htopol
├── thermal_parser.py      # PSRThermalPlant, PSRFuel
├── battery_parser.py      # PSRBateria
├── demand_parser.py       # PSRDemand (block × stage)
├── inflow_parser.py       # PSRGaugingStation historic series
├── stage_parser.py        # study stages, blocks, dates
├── gtopt_writer.py        # thin shim that reuses plp2gtopt writers
├── tests/
│   ├── conftest.py
│   ├── test_*_parser.py   # one per parser
│   └── data/case0/        # vendored PSRClassesInterface case0 (MPL-2.0)
└── DESIGN.md              # this file
```

Most writers (`bus_writer`, `line_writer`, `central_writer`,
`demand_writer`, `cost_writer`, `block_writer`, `stage_writer`,
`generator_profile_writer`, `gtopt_writer`) are **reused unchanged** from
`plp2gtopt`. The intermediate `parsed_data` dict has the same shape;
sddp2gtopt's job is to populate it from PSR collections instead of PLP
`.dat` files. This is the same trick `plp2gtopt`'s `laja_parser` /
`maule_parser` already use for irrigation overlays.

## 4. PSR → gtopt entity mapping

| PSR collection           | gtopt entity                  | Notes                                                    |
|--------------------------|-------------------------------|----------------------------------------------------------|
| `PSRStudy`               | top-level options             | discount_rate, deficit_cost, stage type, initial date    |
| `PSRSystem`              | (no direct equivalent)        | preserved as `area` tag on bus/generator                 |
| `PSRBus`                 | `Bus`                         | direct                                                   |
| `PSRDemand`              | `Demand` + Parquet profile    | block × stage                                            |
| `PSRCircuito`            | `Line`                        | `from`, `to`, capacity, reactance                        |
| `PSRTransformador`       | `Line` (with tap)             | merge with PSRCircuito via PMD `MERGE_CLASS`             |
| `PSRInterconnection`     | `Line` between area buses     | when `use_single_bus=false`                              |
| `PSRThermalPlant`        | `Generator` (thermal)         | + `Fuel` cost segment                                    |
| `PSRFuel`                | (consumed by thermal cost)    | maps to gcost segment                                    |
| `PSRHydroPlant`          | `Reservoir` + `Turbine`       | Vmin/Vmax/Qmin/Qmax/FP/Vinic                             |
| `PSRGaugingStation`      | `Inflow` time series          | Parquet output                                           |
| `htopol.dat`             | hydro topology                | turbine→spill→downstream chain                           |
| `PSRBateria`             | `Battery`                     | direct                                                   |
| `PSRGeneratorUnit`       | `Generator` cluster member    | unit count + per-unit O&M                                |
| `index.dat` (output cat) | gtopt output mapping check    | sanity-check that we cover all 325 SDDP output variables |

Stages (`sddp.dat`): SDDP stage type ∈ {monthly, weekly, hourly} — map to
gtopt `Stage` directly. Blocks (load duration curve) map to gtopt `Block`.
Scenarios = SDDP "series" (hydrological forward sample).

## 5. CLI

Identical surface to `plp2gtopt` where it makes sense:

```
sddp2gtopt INPUT_DIR [-o OUTPUT_DIR] [-y SCENARIOS] [-s LAST_STAGE]
                    [-S sddp|monolithic|cascade] [--info] [--validate]
                    [--use-single-bus] [-l LOG_LEVEL]
```

Drop PLP-specific flags (`--plp-legacy`, `--pmin-as-flowright`,
`--ror-as-reservoirs`, `--expand-water-rights`, `--reservoir-energy-scale`,
…) for v0. Add SDDP-specific ones only when needed (e.g. `--system N` to
filter a multi-system case down to one).

The output-dir inference rule mirrors plp2gtopt: `sddp_<name>` → `gtopt_<name>`.

## 6. Sample case bootstrap

1. Clone `psrenergy/PSRClassesInterface.jl` (MPL-2.0).
2. Vendor `test/data/case0..case5` under
   `scripts/sddp2gtopt/tests/data/` with a `LICENSE` note.
3. Add a `plp2gtopt-run`-style integration test:
   `sddp2gtopt case0 -o /tmp/gtopt_case0 && gtopt --lp-only -p /tmp/gtopt_case0/case0.json`.
4. Once that passes, ask Juan / CEN for a real Chilean SDDP case to
   stress-test multi-system + many hydro plants.

## 7. Phased delivery

| Phase | Deliverable                                                | Gate                                |
|-------|------------------------------------------------------------|-------------------------------------|
| P0    | dir + DESIGN.md + skeleton CLI                             | `sddp2gtopt --help` works           |
| P1    | `psrclasses.json` reader + system / bus / demand / thermal | case0 single-stage LP solves        |
| P2    | hydro (reservoir + topology + inflows)                     | case0 with hydro solves             |
| P3    | network (circuits, transformers, interconnections)         | multi-bus DC OPF on case0           |
| P4    | batteries, scenarios, multi-system                         | case3 / case5 (multi-feature) runs  |
| P5    | `.dat` fallback for cases without `psrclasses.json`        | legacy SDDP install dirs work       |
| P6    | output cross-check vs `index.dat` catalog                  | parity report                       |

Each phase ships with parser + tests + integration test before moving on
(matches `proactive-tests` feedback rule).

## 8. Open questions / risks

- **Currency / units:** SDDP uses k$ and us$/hm³ in places where PLP uses
  $/MWh — confirm gtopt's expected unit at the writer boundary.
- **`hinflw.dat` shape:** is it always `[stage, station, scenario]` or are
  there cases with a different axis order? Check `psrclasses.json`'s
  `PSRGaugingStation.HistoricalInflow` first.
- **`htopol.dat`:** PSR encodes spill+turbine destinations as plant-IDs; PLP
  uses junction names. We need a synthetic-junction generator like
  `JunctionWriter` in plp2gtopt — likely the biggest piece of new code.
- **Multi-system cases:** v0 should accept only single-system; reject
  multi-system with a clear error and add `--system N` later.
- **Inflow scenarios vs SDDP "series":** verify SDDP's stochastic series
  format maps cleanly to gtopt scenario weights.

## 9. Non-goals (v0)

- Reading SDDP binary output (`.bin`/`.hdr`) — that's for `gtopt_compare`,
  not the converter.
- Replicating SDDP's Lagrangean cuts — gtopt has its own SDDP engine.
- Round-trip (gtopt → SDDP) — one-way only.
