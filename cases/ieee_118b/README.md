# `ieee_118b` — IEEE 118-Bus Test Case

The IEEE 118-bus test case represents a portion of the American Electric Power
System (in the Midwestern US) as it was in the early 1960s.  It is the most
widely used medium-scale benchmark in generation and transmission expansion
planning (GTEP) literature.

This case is provided as a **reference system definition** (JSON).  It can be
run manually with gtopt but is not registered as an automated e2e test because
independent golden reference files are not yet available — pandapower's DC OPF
and gtopt's LP formulation produce numerically different solutions for this
size of network.  See "Why gtopt ≠ pandapower" below for the root cause.

## Source

- **Original data**: Iraj Dabbagchi (AEP), converted to IEEE Common Data Format
  by Rich Christie (University of Washington) in August 1993.
- **pglib-opf**: Standardised OPF benchmark with line limits and cost data at
  https://github.com/power-grid-lib/pglib-opf
- **Pandapower**: `pandapower.networks.case118()` (MATPOWER conversion).
- **gtopt JSON**: Generated via `pp2gtopt` from the pandapower network with
  pglib-opf linear cost coefficients added.

## System data

| Quantity | Value |
|----------|-------|
| Buses | 118 |
| Generators | 54 |
| Loads | 99 |
| Branches | 186 (173 lines + 13 transformers) |
| Base voltage | 138 kV / 345 kV |
| Base MVA | 100 |

## Running the case

```bash
gtopt cases/ieee_118b/ieee_118b.json --output-directory /tmp/ieee_118b_out
```

## Why gtopt ≠ pandapower

Running both tools on this case gives different objective values:

| Tool                                         | obj_value |
|----------------------------------------------|----------:|
| `pandapower.networks.case118()` + `pp.rundcopp` | 125 947.87 |
| `gtopt cases/ieee_118b/ieee_118b.json`       | **84 840** |

The 33 % gap is **not a bug** in either tool — they are solving
different problems:

* **pandapower** uses pglib-opf's **quadratic** generator costs:
  `cost(P) = cp1·P + cp2·P²` with `cp1 = 20 $/MWh` (uniform across
  thermals) and `cp2 ∈ [0.016, 0.063]`.  The quadratic term
  incentivises spreading the dispatch across many generators, each
  hitting its individual cost sweet spot.
* **gtopt** is a pure-LP formulation; `Generator.gcost` is a single
  linear coefficient.  `pp2gtopt` collapses the quadratic curve to
  its linear part (`gcost = cp1 = 20`) and discards `cp2`.  Every MW
  from any generator then costs 20 $/MWh, so the LP optimum is
  exactly `20 × 4242 = 84 840` regardless of which generators are
  dispatched (the LP is degenerate at the dispatch level — many
  feasible dispatches, one objective).

The 84 840 figure is therefore the analytical optimum of the
gtopt-linearised LP.  Pinning it as a golden would give us a
correct-by-construction smoke test but not a meaningful cross-tool
check — pandapower simply isn't solving the same problem after the
``pp2gtopt`` lossy conversion.

To restore pandapower-equivalent results, encode the quadratic curve
as a piecewise-linear (PWL) gcost segment list — out of scope for
v0 of ``pp2gtopt``; tracked under the pglib-opf cp2 import roadmap.

## Generating golden files (for future e2e registration)

To register this case as an automated test, golden reference files must be
produced by running gtopt itself on this JSON and copying the output to
`cases/ieee_118b/output/`.  Because different solvers produce different
results for this network, solver-specific golden directories
(`output_cplex/`, `output_highs/`, etc.) should be used:

```bash
gtopt cases/ieee_118b/ieee_118b.json --output-directory /tmp/ieee_118b_out
cp -r /tmp/ieee_118b_out/* cases/ieee_118b/output_highs/
```

Then register with `add_e2e_case(ieee_118b ieee_118b.json)` in
`integration_test/CMakeLists.txt`.
