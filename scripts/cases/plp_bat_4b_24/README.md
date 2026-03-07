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

## Storage Mechanism: ESS (plpess.dat)

This case uses the **ESS mechanism** (`plpess.dat`) rather than the legacy BAT
injection-point mechanism (`plpcenbat.dat`). The flow is:

1. `plpcnfce.dat` defines BESS1 as `Num.BAT=1` (discharge-side generator at bus 3)
2. `plpcenbat.dat` has `NBaterias=0` → `FBaterias=False` → PLP activates ESS mode
3. `plpess.dat` provides the ESS storage properties (energy capacity, efficiency)
4. `plpmaness.dat` is optional (no time-varying ESS maintenance in this case)

## Solar Profile

The solar generator `g_solar` is modelled as a thermal generator with zero cost
and a per-block capacity schedule in `plpmance.dat`. The effective PotMax values
follow the 24-hour solar profile:

```
Profile:  [0, 0, 0, 0, 0, 0, 0.05, 0.15, 0.35, 0.55, 0.75, 0.9, 1.0, 0.95, 0.85, 0.7, 0.5, 0.3, 0.1, 0.02, 0, 0, 0, 0]
PotMax:   [0, 0, 0, 0, 0, 0,  4.5, 13.5, 31.5, 49.5, 67.5,  81,  90,  85.5, 76.5,  63,  45,  27,   9,  1.8, 0, 0, 0, 0] MW
```

## Files

| File | Description |
|------|-------------|
| `plpbar.dat` | 4 buses (b1–b4) |
| `plpblo.dat` | 24 hourly blocks |
| `plpeta.dat` | 1 stage |
| `plpcnfce.dat` | 3 thermal generators + BESS1 (NCenBat=1) + Falla1 |
| `plpcenbat.dat` | NBaterias=0 → triggers ESS mechanism |
| `plpess.dat` | BESS1 ESS properties: 240 MWh, 60 MW, η=0.95 |
| `plpmaness.dat` | No time-varying ESS maintenance |
| `plpcnfli.dat` | 5 transmission lines with DC reactances |
| `plpdem.dat` | 24-block demand profiles for b3 and b4 |
| `plpmance.dat` | Solar profile for g_solar (per-block PotMax) |
| `plpaflce.dat` | No stochastic inflows (0 hydro generators) |
| `plpcosce.dat` | No time-varying costs |
| `plpextrac.dat` | No hydro extraction |
| `plpmanem.dat` | No reservoir maintenance |
| `plpmanli.dat` | No line maintenance |
| `plpcenre.dat` | No variable-efficiency hydro generators |
| `plpcenfi.dat` | No hydro filtration generators |
| `plpcenpmax.dat` | No PotMax vs volume curves |
| `plpidape.dat` | Stochastic apertures (1 simulation, 0 stages) |
| `plpidap2.dat` | Independent hydro apertures (0 stages) |
| `plpidsim.dat` | 1 simulation, 1 etapa |
| `plpfilemb.dat` | No embalse filtrations |
| `plpminembh.dat` | No minimum embalse headwater constraints |
| `plpvrebemb.dat` | No embalse spill volumes |
| `plpplem1.dat` | No future cost function for embalses |
| `plpplem2.dat` | No future cost function gradients |
| `plpdeb.dat` | Simulation parameters |
| `plpmat.dat` | Mathematical solver parameters |
| `plprun.dat` | Warm-start configuration |

## Converting and Running

```bash
# 1. Convert PLP case to gtopt format
plp2gtopt -i scripts/cases/plp_bat_4b_24 -o /tmp/plp_bat_4b_24_gtopt

# 2. Run gtopt on the converted case
cd /tmp/plp_bat_4b_24_gtopt && gtopt bat_4b_24.json

# 3. Compare results with the reference bat_4b_24 case
```

## Relationship to bat_4b_24

This PLP case is the input-format equivalent of `cases/bat_4b_24/bat_4b_24.json`.
Key differences:

- `plp2gtopt` defaults to `use_kirchhoff=false`; add `"use_kirchhoff": true` to
  the options in the converted JSON if DC power flow is desired.
- The solar profile is stored in `Generator/pmax.parquet` (not inline in JSON),
  which is the standard plp2gtopt output for maintenance-derived capacity schedules.
- BESS1 uses the ESS mechanism (`plpess.dat`) rather than the legacy BAT
  injection-point mechanism (`plpcenbat.dat`).
