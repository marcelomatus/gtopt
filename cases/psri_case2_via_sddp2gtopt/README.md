# `psri_case2_via_sddp2gtopt` — multi-system PSR SDDP fixture

End-to-end integration fixture exercising the **multi-system** path
through `scripts/sddp2gtopt/` on the vendored upstream PSR SDDP
sample case2.

## Source

`scripts/sddp2gtopt/tests/data/psri_case2/psrclasses.json` — the
canonical upstream PSR multi-system hydrothermal sample, vendored
from
[`psrenergy/PSRClassesInterface.jl`](https://github.com/psrenergy/PSRClassesInterface.jl)
under MPL-2.0 (see `scripts/sddp2gtopt/tests/data/LICENSE-PSRClassesInterface`).

Composition (per `PSRStudy` / `PSRSystem` collections in the source):

* `NumeroSistemas = 2` → 2 `PSRSystem` entries (North = ref 2000,
  South = ref 2001).
* 3 `PSRThermalPlant` entries, all anchored to system 2000.
* 2 `PSRHydroPlant` entries, all on system 2000.
* 1 `PSRDemand` on system 2000 with 12-month profile.
* System 2001 is empty — no generation, no demand.

## Conversion path

```
scripts/sddp2gtopt/tests/data/psri_case2/psrclasses.json
    │
    │  python -m sddp2gtopt --input-dir … --output-file system_psri_case2_via_sddp2gtopt.json
    ▼
cases/psri_case2_via_sddp2gtopt/system_psri_case2_via_sddp2gtopt.json
```

The committed JSON is the output of the multi-system-aware
`build_planning` after the 2026-05-16 refactor (one `Bus` per
`PSRSystem`, per-spec routing via `system_ref`).  Re-running
`sddp2gtopt` on the upstream PSR fixture produces the same JSON
verbatim (up to dict ordering); the fixture is treated as a
snapshot of "the canonical converter output as of HEAD".

## Expected results (analytical)

`PSRThermalPlant` entries in this upstream case do **not** declare
`G(1)` / `CEsp(1,1)` cost segments — the cost path is via
`PSRFuel.Custo` plus per-plant efficiency, which the v0 converter
does not yet wire through to gtopt's `Generator.gcost`.  So every
thermal lands in the gtopt planning with `gcost = 0.0`, and hydro
plants are flattened to `gcost = 0.0` per the documented v0 hydro
strategy (`gtopt_writer.build_hydro_generators` docstring).

With every generator at `gcost = 0` and a feasible demand of
~17 MW per stage served from 62.5 MW of installed capacity on the
same bus, the optimal dispatch cost is **exactly 0** — that is the
value pinned in `output/solution.csv`.  Solver-internal columns
(`kappa`, `max_kappa`) are zeroed because they have no analytical
counterpart; `tools/gtopt_compare_csv.py` skips them.

System 2001 (`sys_2_bus`) is an empty island — no generators, no
demand — which is trivially feasible.  Until the v4 milestone
adds `PSRInterconnection` → `Line` translation, the two systems
have no transmission link, so their LP subproblems are disjoint.

## CTest coverage

Registered via
`add_e2e_case(psri_case2_via_sddp2gtopt, system_psri_case2_via_sddp2gtopt.json)`
in `integration_test/CMakeLists.txt`.  Produces the standard trio:

* `e2e_psri_case2_via_sddp2gtopt_solve` — runs gtopt.
* `e2e_psri_case2_via_sddp2gtopt_validate_solution` — Python
  `tools/validate_solution.py` checks structure + finite obj.
* `e2e_psri_case2_via_sddp2gtopt_compare_solution` — compares
  produced `solution.csv` against the analytical golden above.

Python-side coverage of the *conversion* step itself lives in
`scripts/sddp2gtopt/tests/test_extended_cases.py::test_multi_system_
cases_convert_to_multi_bus` and
`scripts/sddp2gtopt/tests/test_integration_solver.py` (which
round-trips the smaller `case_two_systems` fixture through the
gtopt binary).

## Cross-tool validation status

**Pandapower DC OPF agrees with gtopt at `obj = 0`** for this
case, but the agreement is shallow: both tools see every generator
at `gcost = 0` (the v0 ``sddp2gtopt`` converter drops the
`PSRFuel.Custo × efficiency` cost path, so a future converter
version that wires fuel costs through to `Generator.gcost` will
break this match).  The cross-tool check here therefore validates
the **multi-bus topology + balance constraints**, not the cost
calculation.

The validation chain for this case is:

1. **Analytical** — `obj = 0` from "all generators have gcost = 0
   and the demand is feasible", documented above.
2. **CTest e2e** — `e2e_psri_case2_via_sddp2gtopt_compare_solution`
   pins the analytical golden.
3. **`gtopt2pp(case) → pp.rundcopp`** — independent solver reports
   the same `obj = 0`; structural sanity check on the multi-bus
   build.

When the v4+ converter lands fuel costs, regenerate the golden
(`obj_value` will become non-zero) and re-confirm pp agreement at
that point.
