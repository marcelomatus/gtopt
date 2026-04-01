# gtopt Technical Brief

> For power system planners, energy researchers, and optimization engineers

## Problem

Power system planners must determine the optimal combination of generation,
transmission, and storage investments to meet growing demand at minimum cost
while integrating variable renewable energy.  This is the **Generation and
Transmission Expansion Planning (GTEP)** problem — a large-scale optimization
challenge that grows combinatorially with system size and planning horizon.

## Solution: gtopt

**gtopt** formulates GTEP as a sparse linear program (LP/MIP) and solves it
with industrial-strength LP backends.  The C++26 implementation provides a
10-27× speedup in LP assembly compared to Python-based tools, enabling
planning studies on larger systems with finer time resolution.

## Mathematical Foundation

The objective minimizes total discounted cost (OPEX + CAPEX):

```
min Σ_s π_s Σ_t δ_t [ Σ_b Δ_b · C_op(s,t,b) + C_inv(t) ]
```

Subject to:
- **Bus power balance** at every node
- **Kirchhoff voltage law** (DC OPF) on every line
- **Generator capacity** bounds and profiles
- **Battery SoC** balance with charge/discharge efficiencies
- **Reservoir volume** balance with hydro cascade topology
- **Reserve requirements** per zone
- **Capacity expansion** with modular investment decisions

## Component Coverage

### Electrical Network
- **Bus**: Electrical nodes with voltage levels and reference angle
- **Generator**: Thermal, solar, wind, hydro — with capacity profiles and variable costs
- **Demand**: Fixed/flexible loads with curtailment penalties
- **Line**: Transmission branches with reactance, transfer limits, losses
- **Battery**: Energy storage with SoC tracking, charge/discharge efficiencies
- **Converter**: Couples batteries to generators (discharge) and demands (charge)

### Hydro System
- **Junction**: Hydraulic nodes in cascade topology
- **Waterway**: Water channels between junctions
- **Reservoir**: Storage lakes with volume balance
- **Turbine**: Links waterway to generator with water-to-power conversion
- **Flow**: Exogenous inflows (rivers, precipitation)
- **Filtration**: Water seepage from waterways to reservoirs

### System Services
- **Reserve Zone**: Spinning reserve requirements (up/down)
- **Reserve Provision**: Generator contributions to reserve zones

## Performance Characteristics

| Metric | gtopt | Typical Python Tool |
|--------|-------|---------------------|
| LP assembly time | < 1 sec | 5-30 sec |
| Memory efficiency | CSC sparse format | Dense arrays |
| Solver interface | Direct C API | Through modeling language |
| I/O format | Native Parquet | CSV/pickle |
| Scalability | 100k+ variables | 10k-50k variables |

## Solver Backends

| Solver | Type | License | Notes |
|--------|------|---------|-------|
| CLP | LP | Open source (EPL) | Default, part of COIN-OR |
| CBC | MIP | Open source (EPL) | Branch-and-cut for integer vars |
| CPLEX | LP/MIP | Commercial (IBM) | Highest performance |
| HiGHS | LP/MIP | Open source (MIT) | Modern parallel solver |

Backends are loaded as dynamic plugins — no recompilation needed to
switch solvers.

## Integration with Existing Workflows

```
                    ┌─────────────┐
  Excel  ──igtopt──→│             │
  PLP   ──plp2gtopt→│   gtopt     │──→ Parquet/CSV results
  pandap ──pp2gtopt─→│  (C++26)   │──→ solution.csv
  JSON   ──direct──→│             │──→ Bus/Gen/Dem/Line outputs
                    └─────────────┘
                          ↑
                    JSON + Parquet
                    input files
```

## Proven on IEEE Benchmarks

Cross-validated against pandapower DC OPF on IEEE 4, 9, 14, 30, and
57-bus networks.  Agreement to solver tolerance (< 10⁻⁶ relative error).

## Contact

- **Repository**: https://github.com/marcelomatus/gtopt
- **License**: BSD-3-Clause
- **Lead Developer**: Marcelo Matus A. (marcelo.matus@usach.cl)
- **Institution**: CENIA / Universidad de Santiago de Chile
