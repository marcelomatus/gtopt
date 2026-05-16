# `fast_hydro_thermal` — SDDP.jl FAST hydro-thermal benchmark

End-to-end integration fixture for gtopt's single-reservoir + thermal
modelling, adapted from the
[SDDP.jl FAST hydro-thermal example](https://sddp.dev/stable/examples/FAST_hydro_thermal/),
which is itself a re-implementation of the
[Cambier "FAST" hydro-thermal example](https://github.com/leopoldcambier/FAST/tree/daea3d80a5ebb2c52f78670e34db56d53ca2e778/examples/hydro%20thermal).

## Source

The reference is SDDP.jl's `FAST_hydro_thermal.jl` (MIT-licensed,
[odow/SDDP.jl](https://github.com/odow/SDDP.jl)):

```julia
@variable(sp, 0 <= x <= 8, SDDP.State, initial_value = 0.0)
@variables(sp, begin
    y >= 0       # hydro outflow → energy
    p >= 0       # thermal generation
    ξ            # random inflow
end)
@constraints(sp, begin
    p + y >= 6                  # demand satisfaction
    x.out <= x.in - y + ξ       # reservoir balance (LE so spill is free)
end)
RAINFALL = (t == 1 ? [6] : [2, 10])
@stageobjective(sp, -5 * p)     # max −5p  ≡  min 5p
```

## Linearisation

SDDP.jl runs a 2-stage stochastic problem with `ξ_2 ∈ {2, 10}` uniformly.
Our gtopt fixture is **deterministic**: we use the **worst-case scenario**
`ξ_1 = 6, ξ_2 = 2` collapsed to a single-stage / 2-block LP.  The
deterministic worst-case (single-scenario) value matches the analytical
optimum of 20 derived below.

For reference, SDDP.jl's stochastic deterministic-equivalent objective is
−10 in max sense (≡ +10 in min sense) — that is the **expectation** over
the two `ξ_2` scenarios.

## Element mapping (SDDP.jl → gtopt)

| SDDP.jl                       | gtopt                                          |
|-------------------------------|------------------------------------------------|
| `x ∈ [0, 8], initial_value 0` | `Reservoir.emin/emax/eini = 0 / 8 / 0`         |
| `y` (hydro outflow)           | `Turbine` on a `Waterway`, drives `g_hydro`    |
| `p` (thermal)                 | `Generator` with `gcost = 5`                   |
| `p + y >= 6`                  | `Demand.capacity = 6` (single-bus balance)     |
| `x.out <= x.in - y + ξ`       | gtopt's built-in reservoir balance + a parallel high-`fmax` spill `Waterway` |
| `ξ` (rainfall)                | `Flow` at the upper junction with deterministic `discharge` |

## Expected results (literature/analytical)

Using `ξ_1 = 6, ξ_2 = 2` (or equivalently `ξ = 4` per-block average — the
LP is degenerate above `y_1 = 2`, see derivation below):

```
block 1:  x_in = 0, ξ_1 = 6   →  y_1 ≤ 6,  p_1 = 6 − y_1
block 2:  x_in = 6 − y_1, ξ_2 = 2  →  y_2 ≤ 8 − y_1  and  y_2 ≤ 6
                                       p_2 = 6 − y_2

cost = 5 (p_1 + p_2) = 5 [(6 − y_1) + (6 − y_2)]

  y_1 ∈ [0, 2]:  y_2 = 6 (full demand-cap), p_2 = 0
                 cost = 5(6 − y_1), min at y_1 = 2: 20
  y_1 ∈ [2, 6]:  y_2 = 8 − y_1, p_2 = y_1 − 2
                 cost = 5(6 − y_1) + 5(y_1 − 2) = 20 (degenerate)
```

Optimal cost = **20** $.  Matches the `obj_value = 20` gtopt writes to
`output/solution.csv`.

## CTest coverage

Registered via `add_e2e_case(fast_hydro_thermal, system_fast_hydro_thermal.json)`
in `integration_test/CMakeLists.txt`.  Same C++ unit-test surfaces as
the Hydro Valley fixture (`Hydro thermal benchmark — SDDP.jl FAST
single-reservoir worst-case` and `Hydro thermal benchmark — FAST hydro-
thermal loaded from JSON literal` in
`test/source/test_hydro_thermal_benchmark.cpp`).

## Cross-tool validation status

**Pandapower DC OPF cannot validate this case.**  The FAST optimum
depends on the inter-block reservoir balance — block 1 deliberately
under-uses hydro to save water for block 2's drought.  Pandapower
is single-snapshot: each block looks independent, hydro appears
"free and plentiful", and `pp.rundcopp` returns `obj = 0` instead
of the analytical 20.  The disagreement is by physics, not by bug
— pp simply isn't solving the same LP.

The validation chain for this case is therefore:

1. **Analytical** — `obj = 20` from the worst-case scenario water-
   balance algebra above.
2. **C++ unit tests** — struct-built and inline-JSON variants in
   `test_hydro_thermal_benchmark.cpp`.
3. **CTest e2e** — `e2e_fast_hydro_thermal_compare_solution` pins
   the analytical golden.

A true cross-tool reference would need a multi-block LP solver such
as JuMP or SDDP.jl running the same deterministic worst-case LP;
not currently implemented.
