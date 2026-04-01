# gtopt — Project Overview

> **High-Performance Generation and Transmission Expansion Planning**

## What is gtopt?

**gtopt** is an open-source, high-performance tool for optimizing power
system investments and operations.  It determines the minimum-cost
combination of:

- **Where to build** new generators, transmission lines, and batteries
- **How much to build** (capacity expansion modules)
- **How to operate** the system hour-by-hour across multiple scenarios

## Key Features

| Feature | Description |
|---------|-------------|
| 🔌 **DC Optimal Power Flow** | Full Kirchhoff voltage-law constraints with network topology |
| 🔋 **Battery Storage** | Charge/discharge efficiency, state-of-charge tracking |
| 💧 **Hydro Cascades** | Reservoirs, turbines, waterways, inflows, and filtration |
| 📈 **Capacity Expansion** | Multi-stage investment decisions with annualized costs |
| 🎲 **Stochastic Planning** | Multiple scenarios with probability weighting |
| ⚡ **High Performance** | C++26 sparse-matrix assembly, 10-27× faster than Python alternatives |
| 🔧 **Pluggable Solvers** | CLP, CBC, CPLEX, HiGHS — auto-detected at runtime |
| 📊 **Apache Parquet I/O** | Native support for large time-series datasets |

## Three Solution Methods

1. **Monolithic LP** — Exact solution for moderate-size problems
2. **SDDP** — Stochastic Dual Dynamic Programming for large multi-scenario problems
3. **Cascade** — Novel multi-level decomposition combining both approaches

## Complete Toolchain

```
Excel Workbook ──→ igtopt ──→ JSON + Parquet ──→ gtopt ──→ Results
PLP Data Files ──→ plp2gtopt ──→ JSON + Parquet ──→ gtopt ──→ Results
pandapower net ──→ pp2gtopt  ──→ JSON + Parquet ──→ gtopt ──→ Results
```

### 16 Python CLI Tools

| Tool | Purpose |
|------|---------|
| `igtopt` | Excel → gtopt converter |
| `plp2gtopt` | PLP/PROMAX → gtopt converter |
| `pp2gtopt` / `gtopt2pp` | pandapower ↔ gtopt converters |
| `gtopt_compare` | Cross-validation against pandapower DC OPF |
| `gtopt_diagram` | Network topology diagram generator |
| `run_gtopt` | Execution manager |
| `sddp_monitor` | SDDP convergence monitoring |
| `gtopt_check_*` | Input validation and diagnostics (4 tools) |
| `cvs2parquet` | CSV → Parquet converter |
| `ts2gtopt` | Time-series → horizon converter |
| `gtopt_field_extractor` | Auto-generate field reference docs |
| `gtopt_compress_lp` | LP file compression |

## Web Interfaces

- **Web Service** (Next.js): REST API + browser UI for submitting and monitoring jobs
- **GUI Service** (Flask): Web-based form interface for creating and editing cases

## Validated on IEEE Benchmarks

| Network | Buses | Result |
|---------|-------|--------|
| IEEE 4-bus | 4 | ✅ Optimal |
| IEEE 9-bus | 9 | ✅ Optimal |
| IEEE 14-bus | 14 | ✅ Optimal |
| IEEE 30-bus | 30 | ✅ Optimal |
| IEEE 57-bus | 57 | ✅ Optimal |

All results cross-validated against pandapower DC OPF to solver tolerance.

## Getting Started

```bash
# Install
cmake -S all -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run a case
./build/standalone/gtopt cases/ieee_9b_ori/ieee_9b_ori.json

# Check results
cat output/solution.csv
cat output/Generator/generation_sol.csv
```

## License

BSD-3-Clause — free for academic and commercial use.

## Links

- **Repository**: https://github.com/marcelomatus/gtopt
- **Documentation**: https://marcelomatus.github.io/gtopt/
- **FESOP Paper**: [IEEE KPEC 2022](https://doi.org/10.1109/KPEC54747.2022.9814781)

---

*gtopt is developed by the Centro Nacional de Inteligencia Artificial
(CENIA), Universidad de Santiago de Chile, with contributions from
Black Bear Engineering, Universidad Nacional de Colombia, and
Imperial College London.*
