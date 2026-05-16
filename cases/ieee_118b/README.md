# `ieee_118b` — IEEE 118-Bus Test Case

The IEEE 118-bus test case represents a portion of the American Electric Power
System (in the Midwestern US) as it was in the early 1960s.  It is the most
widely used medium-scale benchmark in generation and transmission expansion
planning (GTEP) literature.

This case is provided as a **reference system definition** (JSON).  It can be
run manually with gtopt but is not registered as an automated e2e test because
independent golden reference files are not yet available — pandapower's DC OPF
and gtopt's LP formulation produce numerically different solutions for this
size of network.

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
