# PLP Case: plp_bat_4b_24

A PLP-format version of the `cases/bat_4b_24` integration test case. This 4-bus,
24-block case with a battery and a solar generator demonstrates the full
`plp2gtopt` conversion workflow including solar profiles via maintenance schedules.

## System Description

Based on the Grainger & Stevenson 4-bus test network, extended with a battery
storage unit and a solar generator.

| Component | Count | Notes |
|-----------|-------|-------|
| Buses | 4 | b1, b2, b3, b4 |
| Generators | 3 | g1 (250 MW, $20/MWh, b1), g2 (150 MW, $40/MWh, b2), g_solar (90 MW, $0, b1) |
| Demands | 2 | d3 at b3 (peak 110 MW), d4 at b4 (peak 75 MW) — 24-hour profiles |
| Lines | 5 | l1_2, l1_3, l2_3, l2_4, l3_4 |
| Battery | 1 | BESS1 at b3: 200 MWh, 60 MW charge/discharge, η=0.95 |
| Blocks | 24 | Hourly (1 h each), 1 stage, 1 scenario |

## Solar Profile

The solar generator `g_solar` is modelled as a thermal generator with zero cost
and a per-block capacity schedule in `plpmance.dat`. The effective PotMax values
follow the 24-hour solar profile:

```
Profile:  [0, 0, 0, 0, 0, 0, 0.05, 0.15, 0.35, 0.55, 0.75, 0.9, 1.0, 0.95, 0.85, 0.7, 0.5, 0.3, 0.1, 0.02, 0, 0, 0, 0]
PotMax:   [0, 0, 0, 0, 0, 0,  4.5, 13.5, 31.5, 49.5, 67.5,  81,  90,  85.5, 76.5,  63,  45,  27,   9,  1.8, 0, 0, 0, 0]  MW
```

After conversion with `plp2gtopt`, this profile is written to
`Generator/pmax.parquet` and applied as a per-block upper bound on g_solar output.

## Files

| File | Description |
|------|-------------|
| `plpbar.dat` | 4 buses (b1–b4) |
| `plpblo.dat` | 24 hourly blocks |
| `plpeta.dat` | 1 stage |
| `plpcnfce.dat` | 3 generators + BESS1 battery + Falla1 failure generator |
| `plpcenbat.dat` | BESS1 battery configuration (200 MWh, η=0.95, bus b3) |
| `plpcnfli.dat` | 5 transmission lines with reactances matching bat_4b_24 |
| `plpdem.dat` | 24-block demand profiles for b3 and b4 |
| `plpmance.dat` | Solar profile for g_solar (per-block PotMax) |
| `plpaflce.dat` | No stochastic inflows |
| `plpcosce.dat` | No time-varying costs |
| `plpextrac.dat` | No hydro extraction |
| `plpmanem.dat` | No reservoir maintenance |
| `plpmanli.dat` | No line maintenance |

## Converting and Running

```bash
# 1. Convert PLP case to gtopt format
plp2gtopt -i scripts/cases/plp_bat_4b_24 -o /tmp/plp_bat_4b_24_gtopt

# 2. Enable Kirchhoff (DC power flow) via CLI flag
gtopt /tmp/plp_bat_4b_24_gtopt.json --use-kirchhoff --output-directory /tmp/plp_bat_4b_24_results

# 3. Compare results with pandapower reference (if gtopt-compare is available)
gtopt-compare --case bat_4b_24 --gtopt-output /tmp/plp_bat_4b_24_results
```

## Relationship to bat_4b_24

This PLP case is the input-format equivalent of `cases/bat_4b_24/bat_4b_24.json`.
Key differences:

- `plp2gtopt` defaults to `use_kirchhoff=false`; use `--use-kirchhoff` CLI flag
  or add it to the JSON after conversion.
- The solar profile is stored in `Generator/pmax.parquet` (not inline in JSON),
  which is the standard plp2gtopt output for maintenance-derived capacity schedules.
- Bus voltage labels in the bus parser are set to bus numbers (metadata only;
  does not affect the optimization).
