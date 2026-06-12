# `hydro_valley_sddpjl` — SDDP.jl Hydro Valley benchmark

End-to-end integration fixture for gtopt's cascade reservoir + waterway +
turbine + thermal modelling, adapted from the canonical
[SDDP.jl Hydro Valleys example](https://sddp.dev/stable/examples/hydro_valley/)
(MIT-licensed, [odow/SDDP.jl](https://github.com/odow/SDDP.jl)).

## Source

The reference parameters come from SDDP.jl's `hydro_valley_model`
(`docs/src/examples/hydro_valley.jl`):

```julia
valley_chain = [
    Reservoir(0, 200, 200, Turbine([50, 60, 70], [55, 65, 70]), 1000, [0, 20, 50]),
    Reservoir(0, 200, 200, Turbine([50, 60, 70], [55, 65, 70]), 1000, [0,  0, 20]),
]
```

* `Reservoir(min, max, initial, turbine, spill_cost, inflows)`
* `Turbine(flowknots, powerknots)` — piecewise-linear flow→power conversion.

## Linearisation

SDDP.jl runs a 3-stage stochastic problem with a piecewise-linear turbine
curve.  Our gtopt fixture is **deterministic** and uses only the **first
segment** of the piecewise curve (50 m³/h → 55 MW → `production_factor =
1.1`) so the test stays a plain LP (no SOS, no SDDP needed).  Inflows are
collapsed to single deterministic values per junction.

## Element mapping (SDDP.jl → gtopt)

| SDDP.jl                      | gtopt                                           |
|------------------------------|-------------------------------------------------|
| `Reservoir.min / max / initial` | `Reservoir.emin / emax / eini`               |
| `Reservoir.spill_cost`       | implicit — no `spill_cost` field; we model spill as a parallel high-`fmax` `Waterway` |
| `Turbine.flowknots[1]` (50)  | `Waterway.fmax = 70` (a bit above the knot)   |
| `Turbine.powerknots[1] / flowknots[1]` (55/50) | `Turbine.production_factor = 1.1` |
| valley chain (`r→r+1`)       | two `Waterway` per stage (turbine + spill), drained at `Junction.drain = true` |
| stagewise inflows            | one `Flow` per upper-reservoir junction         |

## Expected results (literature/analytical)

* **Demand:** 100 MW × 3 blocks × 1 h = 300 MWh
* **Hydro capacity:** 2 generators × max(turbine flow × production_factor)
  = 2 × 70 × 1.1 = 154 MW per block — well above the 100 MW demand.
* **Reservoirs start full** (`eini = 200` each) and inflows top them up at
  20 + 10 = 30 m³/h.
* All-hydro dispatch is feasible at zero `gcost`, so the optimal objective
  is exactly **0** — and that is what gtopt writes to
  `output/solution.csv` (`obj_value = 0`).

## CTest coverage

Registered via `add_e2e_case(hydro_valley_sddpjl, system_hydro_valley_sddpjl.json)`
in `integration_test/CMakeLists.txt`.  Produces:

* `e2e_hydro_valley_sddpjl_solve` — runs the gtopt binary.
* `e2e_hydro_valley_sddpjl_validate_solution` — checks structure / status.
* `e2e_hydro_valley_sddpjl_compare_<csv>` — per-CSV golden comparison
  against the files in this `output/` directory.

The same fixture is exercised inline by the C++ unit tests
`Hydro thermal benchmark — SDDP.jl Hydro Valley (2-reservoir cascade)` and
`Hydro thermal benchmark — Hydro Valley loaded from JSON literal` in
`test/source/test_hydro_thermal_benchmark.cpp`.

## Cross-tool validation status

**Pandapower DC OPF is not a meaningful cross-check for this case.**
The Hydro Valley LP couples three blocks via the reservoir energy
balance (`x.out = x.in + inflow − turbine − spill`), but
pandapower is a single-snapshot solver — it has no model of
multi-block water storage and would not see the inter-block
constraint that the LP relies on.  Running `gtopt2pp(case) → pp.rundcopp`
happens to return `obj = 0` (same as gtopt's analytical value), but
the match is a coincidence of this specific fixture (hydro covers
the demand at gcost = 0); other reservoir-coupled fixtures
(`fast_hydro_thermal`, `hydro_thermal_sddpjl`) deliberately disagree
with pp by 100 % or more for the same reason.

The validation chain for this case is therefore:

1. **Analytical** — `obj = 0` from "hydro capacity ≫ demand and
   gcost = 0", documented above.
2. **C++ unit tests** — both struct-built and inline-JSON variants
   in `test_hydro_thermal_benchmark.cpp`.
3. **CTest e2e** — `e2e_hydro_valley_sddpjl_compare_solution` pins
   the analytical `obj_value` golden.

A true cross-tool reference would require a JuMP / SDDP.jl helper
that runs the same deterministic LP under a different solver
(planned but not implemented; see the project-level discussion
captured under conversation date 2026-05-16).
