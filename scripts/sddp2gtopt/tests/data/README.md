# sddp2gtopt test fixtures

Each subdirectory is a standalone PSR SDDP case used by the test
suite. Cases come from two sources:

1. **Vendored from `psrenergy/PSRClassesInterface.jl`** under MPL-2.0
   (see `LICENSE-PSRClassesInterface`). These are the canonical
   public PSR sample cases; we keep the upstream `caseN` numbering
   prefixed with `psri_` to avoid clashing with the hand-crafted
   ones.
2. **Hand-crafted** synthetic cases tailored to exercise specific
   code paths (error handling, edge cases, single-feature smoke
   tests). They use the same `psrclasses.json` schema PSR writes
   natively.

## Library

| Case                    | Source       | Size  | What it covers                                              | v0 convertible? |
|-------------------------|--------------|------:|-------------------------------------------------------------|:---------------:|
| `case0/`                | upstream PSR | 32 KB | Real PSR sample (1 sys, 3 thermal, 1 hydro, 2 stations)     | ✅              |
| `psri_case1/`           | upstream PSR | 300 KB| **IEEE-123**: 129 buses, 130 lines, 9 gens, 3 batteries     | ⏳ v3+ (multi-bus) |
| `psri_case2/`           | upstream PSR | 516 KB| **Multi-system** (2 systems), 2 hydros, gen/reserve constraints, ReservoirSet | ⏳ v4+ (multi-system) |
| `psri_case3/`           | upstream PSR | 96 KB | Variant of case2 (smaller `Vazao` series)                   | ⏳ v4+          |
| `case_min/`             | hand-crafted | 1 KB  | Smallest valid case (1 thermal, 1 demand, 1 stage, 1 block) | ✅              |
| `case_thermal_only/`    | hand-crafted | 2 KB  | Thermals only, multi-fuel, 3 stages × 2 blocks              | ✅              |
| `case_two_systems/`     | hand-crafted | 1 KB  | Two `PSRSystem` — must trigger a clear conversion error     | ❌ (rejection)  |
| `case_bad_no_study/`    | hand-crafted | <1 KB | Missing `PSRStudy` — must fail `--validate`                 | ❌ (validation) |
| `case_bad_truncated/`   | hand-crafted | <1 KB | Truncated JSON — must surface a parse error                 | ❌ (parse)      |

## Why three "level-0" PSR cases?

The upstream library named its first sample `case0` (single-bus,
single-system, hydrothermal mini-case) and uses `case1`..`case5` for
progressively bigger ones. We currently vendor case0..case3:

- **`case0`** — mirrors the example in PSR's user manual; single
  bus, deterministic, 2 stages. The v0 acceptance test runs the
  full converter end-to-end on this case.
- **`psri_case1`** — IEEE-123 distribution feeder converted to a
  PSR study. Stresses `PSRBus`, `PSRSerie` (transmission lines),
  `PSRTransformer`, `PSRBattery`, `PSRGenerator`. **No hydro** —
  it's a network-topology stress test.
- **`psri_case2` / `psri_case3`** — two-system hydrothermal cases
  with `PSRInterconnection`, `PSRGenerationConstraintData`,
  `PSRReserveGenerationConstraintData`, `PSRReservoirSet` (alert
  storage curves), and `PSRMaintenanceData`. They define the v4
  multi-system feature set.

Cases 4 and 5 in the upstream repo are *output*-format fixtures
(binary `.bin/.hdr` Graf files), not planning cases, so we don't
vendor them.

## Adding a new fixture

1. Create `case_<name>/psrclasses.json` with at least `PSRStudy` and
   `PSRSystem`.
2. Add a session-scoped fixture in
   [`tests/conftest.py`](../conftest.py) that returns the directory.
3. Reference it from any `test_*.py` by argument name.

The schema is forgiving — collections with no entities are simply
ignored, so the smallest viable fixture is two collections with one
entity each (see `case_min/`). Use the loader's
`PsrClassesLoader.from_case_dir(...)` to read the JSON; downstream
parsers handle missing fields by falling back to dataclass
defaults (see `entities.py`).
