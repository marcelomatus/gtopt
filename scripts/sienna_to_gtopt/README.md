# sienna_to_gtopt

Tiny converter that ports small **NREL-Sienna PowerSimulations.jl** test
cases into gtopt planning JSON, plus three **non-emission** integration
tests that exercise gtopt's hydro-cascade topology, selective
line-monitoring, and HVDC link primitives against a peer reference
implementation.

## Source

* **Framework**: NREL-Sienna's
  [PowerSimulations.jl](https://github.com/NREL-Sienna/PowerSimulations.jl)
  — Julia-based modeling and simulation library for grid operations,
  ~228 named test cases in companion
  [PowerSystemCaseBuilder.jl](https://github.com/NREL-Sienna/PowerSystemCaseBuilder.jl).
* **Datasets**: NREL-Sienna [`PowerSystemsTestData`](https://github.com/NREL-Sienna/PowerSystemsTestData)
  — directories `5-Bus/` and `psy_data/`.
* **Pre-staged bundles** (compressed, shipped in-repo for offline /
  air-gapped CI):
  * `integration_test/data/sienna_5bus.tar.zst` (10.7 KB compressed,
    ~70 KB extracted) — base 5-bus topology + cascade adjacency
    + reserves + storage + time-series pointers
  * `integration_test/data/sienna_14bus.tar.zst` (2.9 KB compressed,
    ~20 KB extracted) — 14-bus per-unit data
  The converter extracts these on-the-fly to a process-local tempdir
  via `_bundle.py::_extract_bundle()` (the OS reclaims the tempdir at
  process exit; one extraction per process is cached).

## Why these three particular cases

The user surveyed Sienna's 228-case catalog and picked the three
highest-marginal-coverage **non-emission** ports for gtopt — the
emission territory is already covered by sister ports
`nrel118_to_gtopt/` and `rts_gmlc_to_gtopt/`. Each variant exercises an
**orthogonal pillar** of gtopt that previously lacked a peer-reviewed
reference benchmark:

| Variant | gtopt feature stressed | Why interesting |
|---|---|---|
| `cascading_hydro` | Junction + Waterway + Turbine + FlowRight + Reservoir cascade topology | gtopt's deepest hydro path (built for Chile/Maule); cascading flow-balance is the hardest topology to get right and no other public LP tool models it the same way |
| `monitored_line` | `Line.enforce_level` knob (0=Never / 1=Voltage / 2=Always) | Sienna's case enforces only one line's thermal limit; direct one-to-one mapping that validates the LP allows overflow on unenforced lines and binds on the monitored one |
| `hvdc` | `Line.dc` flag → no Kirchhoff θ row in the LP | Validates HVDC link handling against Sienna's c_sys5_dc / c_sys14_dc canonical cases |

The Sienna source for each variant:
- `cascading_hydro` ⇔ `c_sys5_hy_cascading_turbine_energy` (per
  `PowerSystemCaseBuilder.jl::src/system_descriptor_data.jl`)
- `monitored_line` ⇔ `c_sys5_ml`
- `hvdc` ⇔ `c_sys5_dc`

## Module layout

```
scripts/sienna_to_gtopt/
  __init__.py                # public API exports
  __main__.py                # `python -m sienna_to_gtopt <variant> -o OUT.json`
  _bundle.py                 # on-the-fly tar.zst extraction (zstandard + tarfile)
  _common.py                 # shared 5-bus base topology builder
  _reader.py                 # parse branch.csv / bus.csv / gen.csv + YAMLs
  _cascading_hydro.py        # cascading_hydro variant builder
  _monitored_line.py         # monitored_line variant builder
  _hvdc.py                   # hvdc variant builder
  tests/
    test_bundle.py           # extraction round-trip + 12-file inventory
    test_reader.py           # CSV + YAML parse → typed dicts
    test_variants.py         # each builder produces a valid gtopt JSON
```

## CLI

```bash
PYTHONPATH=. python -m sienna_to_gtopt cascading_hydro -o cascade.json
PYTHONPATH=. python -m sienna_to_gtopt monitored_line  -o ml.json
PYTHONPATH=. python -m sienna_to_gtopt hvdc            -o hvdc.json
```

## What the integration tests assert

**Files**: `test/source/test_sienna_5bus_{cascading_hydro,monitored_line,hvdc}_port.cpp`
(NOT in `integration_test/` — that subtree is JSON-driven e2e; the C++
TEST_CASEs live under `test/source/` and are auto-discovered via
`CONFIGURE_DEPENDS`, matching the existing port-test pattern of
`test_emission_pjm5_port.cpp` / `test_emission_genx_port.cpp` /
`test_emission_pypsa_port.cpp`).

3 TEST_CASEs per file × 3 files = **9 integration tests** total.

### `cascading_hydro` test asserts
- Flow-balance closure across the 3-reservoir cascade to 1e-6
  numerical tolerance: `Σ inflow + Σ upstream discharge = Σ
  downstream + Σ spillage + storage delta` per stage.
- The LP solves cleanly with `use_single_bus = true` (the 5-bus
  topology has no `line_array` in the cascading-hydro variant, so
  single-bus mode is the natural setup — keeps the cascade as the
  test focus).
- Per-reservoir end-of-horizon volume matches the analytic prediction
  (`vol_end = vol_init + Σ inflow + Σ upstream - Σ discharge`).

### `monitored_line` test asserts
- The single monitored line (`enforce_level = 2` "Always") has its
  thermal cap BINDING in at least one block of the dispatch.
- Unmonitored lines (`enforce_level = 0` "Never") may carry flow
  ABOVE their nominal rating — the LP exploits the unenforced cap
  as PowerSimulations.jl does.
- The overflow ratio (max line flow / rating) is > 1.0 on at least
  one unmonitored line.

### `hvdc` test asserts
- The HVDC line (`dc = true`) contributes ZERO θ-coupling rows to the
  LP. The test compares all-AC LP row count vs one-DC LP row count
  and verifies the delta matches the number of skipped KVL equations.
- AC lines on the same 5-bus topology STILL contribute KVL rows
  (regression: turning one line DC doesn't break the others).

## Surprises in Sienna's data conventions

Three conventions tripped the converter during development; documented
here so future ports avoid the same potholes:

1. **`var_cost_0` is a no-load anchor, not a segment marginal slope.**
   Sienna's `gen.csv` ships cost as a polynomial anchor convention:
   `var_cost_0` is the y-intercept (often 0), and `var_cost_1` /
   `var_cost_2` are the marginal `$/MWh` slopes on subsequent segments.
   Naively using `var_cost_0` (as the PLEXOS / PLP convention would
   suggest — "first segment is the marginal rate") gives 0 cost for
   every NG unit — completely broken merit order. The converter's
   `_reader.py` falls through `var_cost_1 → var_cost_0 → VOM` so the
   conversion is robust to either layout.

2. **`Hydro_Upstream_Input.csv` renames `HydroDispatch{1..3}` to
   `HydroUnit{1..3}` without a join key.** The cascade adjacency table
   lives in a separate namespace from `gen.csv` — the converter pairs
   them by ordinal position only, mirroring how PowerSimulations.jl's
   own `modifier.jl` reassembles the cascade. Any future Sienna data
   format change that breaks this pairing will need explicit name
   normalization.

3. **No `line_array` in some 5-bus variants.** The cascading-hydro
   variant ships zero transmission lines (the cascade is the entire
   network). Without `use_single_bus = true`, the 5 buses are
   topologically disconnected and load buses with no thermal gen
   force demand-fail. Defaults to single-bus for cascade variants;
   keeps the cascade topology as the test focus.

## Related work

- Sister ports `scripts/nrel118_to_gtopt/` (NREL-118 with IPCC factors)
  and `scripts/rts_gmlc_to_gtopt/` (RTS-GMLC with per-gen native
  multi-pollutant CO₂) cover the emissions axis.
- The pre-staged bundles live at `integration_test/data/` — see that
  directory's README for the inventory + regen commands.
- Issue [#510](https://github.com/marcelomatus/gtopt/issues/510) covers
  the multi-fuel / TB-schedule-fuel C++ work that would unlock
  piecewise-emission ports without the scalar-collapse workaround the
  emission ports use today (not relevant to these three non-emission
  variants).
