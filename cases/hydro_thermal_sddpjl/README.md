# `hydro_thermal_sddpjl` — SDDP.jl Hydro_thermal benchmark

End-to-end integration fixture for gtopt's single-reservoir + thermal
modelling, adapted from the canonical
[SDDP.jl Hydro_thermal example](https://sddp.dev/stable/examples/Hydro_thermal/)
(MIT-licensed, [odow/SDDP.jl](https://github.com/odow/SDDP.jl)).

## Source

SDDP.jl `docs/src/examples/Hydro_thermal.jl`:

```julia
graph = SDDP.UnicyclicGraph(0.95; num_nodes = 3)
model = SDDP.PolicyGraph(graph; sense = :Min, lower_bound = 0.0, …) do sp, t
    @variable(sp, 5 <= x <= 15, SDDP.State, initial_value = 10)
    @variable(sp, g_t >= 0)
    @variable(sp, g_h >= 0)
    @variable(sp, s >= 0)
    @constraint(sp, balance, x.out - x.in + g_h + s == w_i)
    @constraint(sp, demand,  g_h + g_t == w_d)
    @stageobjective(sp, s + t * g_t)
    SDDP.parameterize(sp, [[0, 7.5], [3, 5], [10, 2.5]]) do w …
end
```

* Reservoir state `x ∈ [5, 15]`, `x_0 = 10`.
* Per-stage stage-cost coefficient `t ∈ {1, 2, 3}` on thermal `g_t`.
* Stagewise-independent random `(w_i, w_d)` drawn uniformly from
  `{(0, 7.5), (3, 5), (10, 2.5)}`.

## Linearisation

We use the **worst-case scenario** `(w_i = 0, w_d = 7.5)` every block
across a 3-block single-stage LP, with stage-cost folded to a uniform
`t = 1` so the objective collapses to `s + g_t`.  This collapses the
stochastic infinite-horizon problem into a single deterministic LP
whose optimum can be derived analytically.

## Element mapping

| SDDP.jl                       | gtopt                                          |
|-------------------------------|------------------------------------------------|
| `x ∈ [5, 15]`, `x_0 = 10`     | `Reservoir.emin/emax/eini = 5 / 15 / 10`       |
| `g_h` (hydro generation)      | `Turbine` on a `Waterway`, drives `g_hydro`    |
| `g_t` (thermal generation)    | `Generator gcost = 1`                          |
| `g_h + g_t = w_d`             | `Demand.capacity = 7.5` + `demand_fail_cost = 1000` |
| `x.out - x.in + g_h + s = w_i` | gtopt's built-in reservoir balance + parallel high-`fmax` spill `Waterway` |
| `w_i` (rainfall)              | `Flow` with `discharge = 0` (worst-case)       |

## Expected objective (literature/analytical)

gtopt's reservoir applies a `flow_conversion_rate` (default
`3.6` — see `include/gtopt/reservoir_lp.hpp:53`) when converting
turbine flow in m³/s to per-hour reservoir depletion.  Per-block
draw is then capped at `(eini − emin) / 3.6 = 5 / 3.6 ≈ 1.3889`
m³/s.  With `production_factor = 1.0` the per-block hydro output
ceiling is the same `1.3889` MW.

```
block 1:  x_in = 10 (eini).  g_h = 1.3889, s = 0, g_t = 6.1111.
          cost_1 = g_t = 6.1111.  x_out = 5.
block 2:  x_in = 5 (emin) and w_i = 0 → no headroom.
          g_h = 0, s = 0, g_t = 7.5.  cost_2 = 7.5.
block 3:  same as block 2 → cost_3 = 7.5.
```

**Optimal cost = 6.1111 + 7.5 + 7.5 = 22.5 − 5/3.6 = 21.1111…**

This is the value pinned in `output/solution.csv` for the
`e2e_hydro_thermal_sddpjl_compare_solution` test.  Solver-internal
columns (`kappa`, `max_kappa`) are zeroed because no analytical
derivation produces them; `tools/gtopt_compare_csv.py` already
skips them.

## CTest coverage

Registered via
`add_e2e_case(hydro_thermal_sddpjl, system_hydro_thermal_sddpjl.json)`
in `integration_test/CMakeLists.txt`.  The companion C++
`Hydro thermal benchmark — Hydro_thermal loaded from JSON literal`
test in `test/source/test_hydro_thermal_benchmark.cpp` exercises the
same fixture inline; the e2e case adds the disk-loaded JSON +
golden-CSV path.

## Cross-tool validation status

**Pandapower DC OPF cannot validate this case.**  The 21.111…
optimum hinges on the gtopt-specific `flow_conversion_rate = 3.6`
factor on the `Reservoir` element (m³/s → per-hour reservoir
depletion).  Pandapower is single-snapshot, has no `Reservoir`
element, and reports `obj = 0` (free hydro at gcost = 0).  The
gap from `21.111` to `0` is by physics, not bug.

The validation chain for this case is:

1. **Analytical** — `obj = 22.5 − 5/3.6 = 21.111…` from the
   block-by-block algebra above, derived from the `flow_conversion_
   rate` source comment in `include/gtopt/reservoir_lp.hpp:53`.
2. **C++ unit test** — `Hydro thermal benchmark — Hydro_thermal
   loaded from JSON literal` (no struct-built variant — see
   header comment).
3. **CTest e2e** — `e2e_hydro_thermal_sddpjl_compare_solution`
   pins the analytical golden.

A meaningful cross-tool reference would need a tool that models
gtopt's specific reservoir-flow conversion + emin/eini bounds (so
neither SDDP.jl nor pandapower works directly).  The closest
deterministic LP comparison would be a hand-rolled JuMP model
with the same conversion factor; not currently implemented.
