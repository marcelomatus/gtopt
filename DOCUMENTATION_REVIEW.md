# Documentation Structure Review and Recommendations

> **Status as of 2026-02**: Items marked ✅ have been implemented. Items marked
> ⬜ remain as future recommendations.

This document provides a comprehensive review of the gtopt documentation
structure and suggests improvements for better organization, clarity, and
maintainability.

## Current Documentation Structure

### Root Level Documentation
- **README.md** – Main project overview and quick start guide
- **BUILDING.md** – ✅ Detailed build instructions for all platforms
- **USAGE.md** – Comprehensive usage guide with examples
- **INPUT_DATA.md** – ✅ Input data structure reference (moved from `guiservice/`)
- **CONTRIBUTING.md** – ✅ Contribution guidelines including code style, CI, testing
- **CLAUDE.md** – ✅ Developer guidance for Claude Code (AI assistant)
- **.github/copilot-instructions.md** – ✅ Comprehensive GitHub Copilot agent guide

### Service-Specific Documentation
- **webservice/README.md** – Web service overview
- **webservice/INSTALL.md** – Detailed installation and deployment guide
- **guiservice/README.md** – GUI service overview
- **guiservice/INSTALL.md** – Detailed installation and deployment guide

### Technical Documentation
- **test/BENCHMARK_RESULTS.md** – Performance benchmarks (`flat_map` vs `std::map`)

## Issues and Recommendations

### 1. Redundant "Running the Sample Case" Content ⬜
**Issue**: The "Running the Sample Case" section appears in both README.md and
USAGE.md with similar content.

**Recommendation**:
- Keep a minimal example in README.md (just the command and expected output)
- Move all detailed explanations to USAGE.md
- Add a clear reference from README.md to USAGE.md for more details

### 2. Documentation Discovery Path ✅ (partially)
**Status**: README.md already has a "Documentation Guide" section listing all
documents. The `.github/copilot-instructions.md` provides a comprehensive
reference for agents.

### 3. INPUT_DATA.md Location ✅
**Status**: `INPUT_DATA.md` has been moved to the root level.
`guiservice/README.md` references `../INPUT_DATA.md`.

### 4. CONTRIBUTING.md ✅
**Status**: CONTRIBUTING.md created and updated with:
- Correct Python line length (88, not 120)
- Correct naming conventions (`snake_case` functions, `m_<name>_` private members)
- Clang 21 as preferred compiler
- Correct primary build target (`cmake -S test -B build`)
- Domain context section
- Full testing guidelines

### 5. CLAUDE.md and copilot-instructions.md ✅
**Status**: Both files expanded with:
- Environment setup (APT, conda, Clang 21)
- Correct naming conventions
- GTEP domain knowledge
- IEEE benchmark case descriptions and expected outputs
- LP formulation summary
- Comparable tools section (PyPSA, pandapower, GenX, QuESt Planning)
- Domain glossary

### 6. Missing API Documentation ⬜
**Issue**: No consolidated API reference; each service has its own inline docs.
**Recommendation**: Consider OpenAPI/Swagger specs for the webservice REST API
and the guiservice Flask endpoints.

### 7. CHANGELOG.md ⬜
**Issue**: No version history document.
**Recommendation**: Add `CHANGELOG.md` to track breaking changes and migrations.

### 8. ARCHITECTURE.md ⬜
**Issue**: No architecture overview document.
**Recommendation**: Document the LP assembly pipeline:
`Planning → PlanningLP → SimulationLP → SystemLP → LinearProblem → FlatLinearProblem → CBC/HiGHS`
and the hydro cascade model.

### 9. Tutorials ⬜
**Issue**: No step-by-step tutorials for common workflows.
**Recommendation**: Add `docs/tutorials/` with:
- `basic-thermal.md` — single-bus dispatch
- `multi-bus-kirchhoff.md` — DC OPF with IEEE 9-bus
- `expansion-planning.md` — multi-stage CAPEX optimization
- `hydro-cascade.md` — hydro turbine and reservoir modeling
- `battery-storage.md` — battery + converter modeling

## Implementation Priority

### High Priority (Immediate)
1. ✅ Create BUILDING.md
2. ✅ Move INPUT_DATA.md to root
3. ✅ Create/update CONTRIBUTING.md
4. ✅ Expand CLAUDE.md and copilot-instructions.md
5. ⬜ Simplify README.md usage section to reduce redundancy with USAGE.md

### Medium Priority (Near Term)
1. ⬜ Add CHANGELOG.md
2. ⬜ Add ARCHITECTURE.md
3. ⬜ OpenAPI spec for webservice REST API

### Low Priority (Long Term)
1. ⬜ Create tutorial documentation
2. ⬜ Doxygen HTML generation and hosting

