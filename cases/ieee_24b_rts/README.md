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

## System data

| Quantity | Value |
|----------|-------|
| Buses | 24 |
| Generators | 32 |
| Loads | 17 |
| Branches | 38 |
| Base voltage | 138 kV / 230 kV |
| Base MVA | 100 |

## Generating golden files

```python
import pandapower as pp
import pandapower.networks as pn

net = pn.case24_ieee_rts()

# Add linear generation costs (see pglib-opf for reference values)
# Or convert from pglib-opf MATPOWER file:
# python -m pp2gtopt pglib_opf_case24_ieee_rts.m -o cases/ieee_24b_rts/ieee_24b_rts.json

pp.rundcopp(net)
# Write net.res_* to cases/ieee_24b_rts/output/ as gtopt CSV files
```

## CTest coverage

Registered via `add_e2e_case(ieee_24b_rts, ieee_24b_rts.json)` in
`integration_test/CMakeLists.txt`.
