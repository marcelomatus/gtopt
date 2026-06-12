# `ieee_24b_rts` — IEEE Reliability Test System (24-Bus)

The IEEE RTS-96 (24-bus) is a reliability-focused test system developed by the
IEEE Reliability Test System Task Force.  It includes generator availability,
transmission outages, and multi-area configurations, making it a standard
benchmark for reliability-constrained GTEP and stochastic planning.

## Source

- **Original**: IEEE RTS Task Force, "The IEEE Reliability Test System — 1996",
  IEEE Trans. Power Systems, 1999.
- **Pandapower**: `pandapower.networks.case24_ieee_rts()`.
- **pglib-opf**: Standardised OPF version with cost data at
  https://github.com/power-grid-lib/pglib-opf

## System data (as ingested by `pp2gtopt`)

| Quantity | Value |
|----------|-------|
| Buses | 24 |
| Generators | 11 (10 `gen` + 1 `ext_grid` → slack-gen) |
| Loads | 17 |
| Branches | 38 |
| Base voltage | 138 kV / 230 kV |
| Base MVA | 100 |
| Total `pmax` capacity | **1 509 MW** |
| Total `Pd` demand | **2 850 MW** |

## Generating the gtopt JSON

```python
import pandapower as pp, pandapower.networks as pn
from pp2gtopt.convert import convert as pp2gt
from pathlib import Path

net = pn.case24_ieee_rts()
pp2gt(
    output_path=Path("cases/ieee_24b_rts/ieee_24b_rts.json"),
    net=net,
    name="ieee_24b_rts",
    solver_type="mono",
)
```

## Why gtopt obj is dominated by demand-fail

`pandapower.networks.case24_ieee_rts` ships a deliberately under-sized
generation set: total `max_p_mw` = 1 509 MW vs total `Pd` = 2 850 MW.
Pandapower's own `pp.rundcopp` does **not** flag this as infeasible —
`pp.res_load["p_mw"]` echoes the input demand (2 850 MW) while
`pp.res_gen["p_mw"].sum() + pp.res_ext_grid["p_mw"].sum() ≈ 1 327`,
leaving a silent 1 522 MW imbalance.  The `pp.res_cost ≈ 61 001` it
reports is therefore the cost of the partial dispatch only and is
**not** a balanced LP value — useful for unit-testing the cost
calculator, not as a cross-tool reference.

gtopt's LP, by contrast, enforces the balance constraint strictly
and routes the shortfall through the `Demand` element's `fail`
variable at `demand_fail_cost = 1 000` $/MWh:

| Term | Quantity | Coefficient | Subtotal |
|---|---:|---:|---:|
| Dispatched gens (cheapest-first up to 1 509 MW capacity) | 1 509 MW | varied gcost ∈ {0, 0.001, 4.42, 12.39, 43.66, …} | **27 194.16** |
| Unserved demand (`fail` variable) | 1 341 MW | `demand_fail_cost = 1 000` | **1 341 000.00** |
| **Total**                                          |   |   | **1 368 194.16** |

The golden `obj_value = 1 368 194.16` in `output/solution.csv` is this
analytical sum.  Per-element CSV goldens (`Bus/`, `Generator/`,
`Demand/fail_sol`, etc.) are not pinned — the dispatch is degenerate
above the gcost = 4.42 tier (multiple gens at the same cost) so any
solver-deterministic per-MW assignment would be a snapshot test, not
an analytical one.

This case therefore exercises gtopt's **demand-fail accounting path**
end-to-end on a 24-bus topology — the only e2e fixture in the suite
that does so on a non-trivial network.

## CTest coverage

Registered via `add_e2e_case(ieee_24b_rts, ieee_24b_rts.json)` in
`integration_test/CMakeLists.txt`.  Produces the standard trio
(`_solve` / `_validate_solution` / `_compare_solution`).

## Pandapower cross-tool status

Not registered for `add_pandapower_comparison(...)`.  See "Why gtopt
obj is dominated by demand-fail" above — pp silently masks the
imbalance, so a cross-tool match would be misleading.  Once a future
`pp2gtopt` (or pglib-opf importer) lands cost data **and** balanced
capacity, the case can be re-baselined against pp's DC OPF.
