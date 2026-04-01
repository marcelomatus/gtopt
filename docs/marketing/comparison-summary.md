# Tool Comparison Summary: gtopt vs Alternatives

> Quick reference for evaluating gtopt against similar tools

## Feature Matrix

| Feature | **gtopt** | **PyPSA** | **GenX** | **PLP** | **pandapower** | **IDAES-GTEP** | **QuESt** |
|---------|-----------|-----------|----------|---------|----------------|----------------|-----------|
| **Language** | C++26 | Python | Julia | Fortran | Python | Python | Python |
| **License** | BSD-3 | MIT | GPL-2 | Proprietary | BSD-3 | BSD-3 | BSD-3 |
| **DC OPF (Kirchhoff)** | ✅ | ✅ | Transport only | ✅ | ✅ | ✅ | ❌ |
| **AC OPF** | ❌ | ✅ | ❌ | ❌ | ✅ | ✅ | ❌ |
| **Hydro cascades** | ✅ Full | Partial | ❌ | ✅ Full | ❌ | ❌ | ❌ |
| **Battery storage** | ✅ | ✅ | ✅ | Partial | ✅ | Partial | ✅ |
| **Capacity expansion** | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ | ✅ |
| **SDDP solver** | ✅ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ |
| **Multi-scenario** | ✅ | Partial | Partial | ✅ | ❌ | ❌ | ❌ |
| **Multi-sector** | ❌ | ✅ (gas,heat) | ❌ | ❌ | ❌ | ❌ | ❌ |
| **Parquet I/O** | ✅ Native | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **Web GUI** | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
| **Excel input** | ✅ (igtopt) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **Spinning reserves** | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ | ❌ |
| **Phase-shift transformers** | ✅ | ✅ | ❌ | ✅ | ✅ | ❌ | ❌ |

## When to Use Each Tool

### Use **gtopt** when you need:
- High-performance LP assembly for large-scale GTEP
- Complete hydro cascade modeling (reservoirs, turbines, filtration)
- SDDP for multi-stage stochastic problems
- Native Parquet I/O for big data workflows
- Migration from PLP/PROMAX (use `plp2gtopt`)
- Cross-validation with pandapower (use `gtopt_compare`)

### Use **PyPSA** when you need:
- Multi-sector coupling (electricity + gas + heat + transport)
- Large community and ecosystem
- AC power flow analysis
- Familiar Python API

### Use **GenX** when you need:
- Julia/JuMP ecosystem integration
- Advanced operational constraints (unit commitment)
- US-scale capacity expansion studies

### Use **pandapower** when you need:
- AC power flow and state estimation
- Short-circuit analysis
- Network topology analysis and visualization
- Quick power flow studies (no expansion planning)

### Use **PLP** when you need:
- Legacy Latin American hydrothermal scheduling
- Established regulatory acceptance in Chile/Colombia/Brazil

## Performance Comparison

| Aspect | gtopt (C++) | Python Tools | Julia Tools |
|--------|-------------|-------------|-------------|
| LP assembly | ~0.1-1 sec | ~5-30 sec | ~1-5 sec |
| Memory | Sparse CSC | Dense numpy | Sparse JuMP |
| Solver call | Direct C API | Through Pyomo/linopy | Through JuMP |
| Startup time | < 1 sec | 2-5 sec (imports) | 5-30 sec (JIT) |
| Parquet I/O | Native Arrow C++ | Via pyarrow | Via Arrow.jl |

## Conversion Paths

```
PLP (Fortran) ──plp2gtopt──→ gtopt (C++)
pandapower    ──pp2gtopt───→ gtopt (C++) ──gtopt2pp──→ pandapower
Excel         ──igtopt─────→ gtopt (C++)
```

All converters are included in the gtopt repository under `scripts/`.

## References

- **gtopt**: https://github.com/marcelomatus/gtopt
- **PyPSA**: Brown et al. (2018), J. Open Res. Softw., DOI: 10.5334/jors.188
- **GenX**: Jenkins & Sepulveda (2017), MIT Energy Initiative
- **pandapower**: Thurner et al. (2018), IEEE Trans. Power Syst., DOI: 10.1109/TPWRS.2018.2829021
- **FESOP/gtopt**: Buitrago et al. (2022), IEEE KPEC, DOI: 10.1109/KPEC54747.2022.9814781
