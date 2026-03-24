# Documentation Structure Review and Recommendations

> **Status as of 2026-03**: Items marked ✅ have been implemented. Items marked
> ⬜ remain as future recommendations.

This document provides a comprehensive review of the gtopt documentation
structure and suggests improvements for better organization, clarity, and
maintainability.

## Current Documentation Structure

### Root Level Documentation
- **README.md** – ✅ Main project overview, quick start, Python scripts table (15 tools), and related tools
- **BUILDING.md** – ✅ Detailed build instructions for all platforms
- **USAGE.md** – ✅ Comprehensive usage guide with examples
- **INPUT_DATA.md** – ✅ Input data structure reference (moved from `guiservice/`)
- **CONTRIBUTING.md** – ✅ Contribution guidelines including code style, CI, testing
- **CLAUDE.md** – ✅ Developer guidance for Claude Code (AI assistant)
- **.github/copilot-instructions.md** – ✅ Comprehensive GitHub Copilot agent guide
- **SCRIPTS.md** – ✅ All 15 Python tools documented with CLI references
- **DIAGRAM_TOOL.md** – ✅ Network and planning diagram tool with examples for all case types
- **USER_CONSTRAINTS.md** – ✅ User-defined LP constraints syntax and reference

### Technical Documentation
- **docs/formulation/MATHEMATICAL_FORMULATION.md** – ✅ Full LP/MIP formulation
  with LaTeX notation, JSON field mapping, and academic references
- **docs/scripts/*.md** – ✅ Per-script detailed documentation (7 files)
- **docs/diagrams/*.mmd** – ✅ Mermaid diagrams for system topology and planning
- **docs/templates/gtopt_template.xlsx** – ✅ Excel template for igtopt
- **docs/SDDP_SOLVER.md** – ✅ SDDP solver documentation
- **docs/CASCADE_SOLVER.md** – ✅ Cascade solver documentation
- **docs/MONOLITHIC_SOLVER.md** – ✅ Monolithic solver documentation
- **docs/TOOL_COMPARISON.md** – ✅ Comparison with PLP, pandapower, etc.
- **test/BENCHMARK_RESULTS.md** – Performance benchmarks (`flat_map` vs `std::map`)

### Service-Specific Documentation
- **webservice/README.md** – Web service overview
- **webservice/INSTALL.md** – Detailed installation and deployment guide
- **guiservice/README.md** – GUI service overview
- **guiservice/INSTALL.md** – Detailed installation and deployment guide

## Issues and Recommendations

### 1. Redundant "Running the Sample Case" Content ⬜
**Issue**: The "Running the Sample Case" section appears in both README.md and
USAGE.md with similar content.

**Recommendation**:
- Keep a minimal example in README.md (just the command and expected output)
- Move all detailed explanations to USAGE.md
- Add a clear reference from README.md to USAGE.md for more details

### 2. Documentation Discovery Path ✅
**Status**: README.md has a "Documentation Guide" section listing all
documents with organized links.  The `.github/copilot-instructions.md`
provides a comprehensive reference for agents.

### 3. INPUT_DATA.md Location ✅
**Status**: `INPUT_DATA.md` has been moved to the root level.
`guiservice/README.md` references `../INPUT_DATA.md`.

### 4. CONTRIBUTING.md ✅
**Status**: CONTRIBUTING.md created and updated with:
- Correct Python line length (88, not 120)
- Correct naming conventions (`snake_case` functions, `m_<name>_` private members)
- Clang 21 as preferred compiler
- Correct primary build target (`cmake -S all -B build`)
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
- Documentation style guide for future updates

### 6. Academic References in Mathematical Formulation ✅
**Status**: `docs/formulation/MATHEMATICAL_FORMULATION.md` updated with:
- 16 academic references organized by category (FESOP/Matus publications,
  TEP classics, DC OPF, similar tools, solvers, surveys)
- Inline citation markers throughout all formulation sections
- Cross-references validated against C++ implementation
- Formulation verified consistent with established literature:
  - DC power flow: standard $f = B(\theta_a - \theta_b)$ with angle scaling
  - Battery SoC: standard linear storage model with round-trip efficiency
  - Bus balance: KCL with injection/withdrawal loss factors
  - Capacity expansion: modular LP/MIP with derating and inter-stage linking

### 7. Python Scripts Documentation ✅
**Status**: All 15 Python scripts are now documented in `SCRIPTS.md`:
- **Previously documented** (7): `gtopt_diagram`, `plp2gtopt`, `pp2gtopt`,
  `igtopt`, `cvs2parquet`, `ts2gtopt`, `gtopt_compare`
- **Newly documented** (8): `run_gtopt`, `gtopt_check_json`, `gtopt_check_lp`,
  `gtopt_check_output`, `gtopt_compress_lp`, `gtopt2pp`, `gtopt_config`,
  `sddp_monitor` (expanded)
- Each new tool section includes: purpose, basic usage, CLI options table,
  and integration notes

### 8. Missing API Documentation ⬜
**Issue**: No consolidated API reference; each service has its own inline docs.
**Recommendation**: Consider OpenAPI/Swagger specs for the webservice REST API
and the guiservice Flask endpoints.

### 9. CHANGELOG.md ✅
**Status**: `CHANGELOG.md` created with version history and notable changes.

### 10. ARCHITECTURE.md ⬜
**Issue**: No architecture overview document.
**Recommendation**: Document the LP assembly pipeline:
`Planning → PlanningLP → SimulationLP → SystemLP → LinearProblem → FlatLinearProblem → CBC/HiGHS`
and the hydro cascade model.

### 11. Tutorials ⬜
**Issue**: No step-by-step tutorials for common workflows.
**Recommendation**: Add `docs/tutorials/` with:
- `basic-thermal.md` — single-bus dispatch
- `multi-bus-kirchhoff.md` — DC OPF with IEEE 9-bus
- `expansion-planning.md` — multi-stage CAPEX optimization
- `hydro-cascade.md` — hydro turbine and reservoir modeling
- `battery-storage.md` — battery + converter modeling

### 12. Diagram Tool Examples ✅
**Status**: `DIAGRAM_TOOL.md` expanded with examples covering all case types:
- Small IEEE cases (4-bus, 9-bus, 14-bus)
- Medium IEEE cases (30-bus, 57-bus)
- Battery and storage cases
- Expansion planning cases
- SDDP and multi-phase cases
- Mermaid embedding examples

## Implementation Priority

### High Priority (Immediate)
1. ✅ Create BUILDING.md
2. ✅ Move INPUT_DATA.md to root
3. ✅ Create/update CONTRIBUTING.md
4. ✅ Expand CLAUDE.md and copilot-instructions.md
5. ✅ Document all Python scripts in SCRIPTS.md
6. ⬜ Simplify README.md usage section to reduce redundancy with USAGE.md

### Medium Priority (Near Term)
1. ✅ Academic references in mathematical formulation
2. ✅ CHANGELOG.md
3. ⬜ Add ARCHITECTURE.md
4. ⬜ OpenAPI spec for webservice REST API

### Low Priority (Long Term)
1. ⬜ Create tutorial documentation
2. ⬜ Doxygen HTML generation and hosting
