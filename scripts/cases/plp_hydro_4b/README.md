# PLP Hydro 4-Bus Test Case (`plp_hydro_4b`)

Small hydrothermal power system test case for PLP-to-gtopt integration testing.

## System Description

- **4 buses**: b1 (hydro hub), b2 (run-of-river), b3 (demand), b4 (demand)
- **5 transmission lines**: l1_2, l1_3, l2_3, l2_4, l3_4 (100–200 MW capacity)
- **1 reservoir** (LakeA): 100 MW turbine, 100–1200 Mm³ storage, Vini=600 Mm³, Vfin=500 Mm³
- **1 turbine** (TurbineA): 100 MW series hydro downstream of LakeA
- **1 run-of-river** (HydroRoR): 50 MW at b2
- **1 thermal generator** (Thermal1): 200 MW at b2, cost 50 $/MWh
- **1 failure/penalty** generator (Falla1): 9999 MW, cost 500 $/MWh

## Time Structure

- **3 stages** (monthly): 12 hours each
- **3 blocks per stage**: 4 hours each (9 blocks total)
- **3 hydrologies**: wet (H1), normal (H2), dry (H3)
- **3 apertures** per stage (matching hydrologies)

## Demand

| Bus | Block 1-3 range | Block 4-6 range | Block 7-9 range |
|-----|-----------------|-----------------|-----------------|
| b3  | 80–90 MW        | 75–85 MW        | 70–80 MW        |
| b4  | 50–60 MW        | 45–55 MW        | 40–50 MW        |

## Inflows (m³/s to LakeA)

| Block  | Wet (H1) | Normal (H2) | Dry (H3) |
|--------|----------|-------------|-----------|
| 1      | 40.0     | 30.0        | 20.0      |
| 2      | 45.0     | 32.0        | 22.0      |
| 3      | 42.0     | 31.0        | 21.0      |
| 4      | 35.0     | 25.0        | 15.0      |
| 5      | 38.0     | 27.0        | 17.0      |
| 6      | 36.0     | 26.0        | 16.0      |
| 7      | 50.0     | 35.0        | 25.0      |
| 8      | 55.0     | 38.0        | 28.0      |
| 9      | 52.0     | 36.0        | 26.0      |

## Purpose

This case is designed for comparing PLP SDDP solver results with gtopt
monolithic/SDDP solver results. It tests:

1. Reservoir volume management (Vini → Vfin trajectories)
2. Hydro dispatch with stochastic inflows
3. Multi-bus DC power flow with Kirchhoff constraints
4. Marginal cost comparison between PLP and gtopt
5. Aperture-based stochastic optimization
