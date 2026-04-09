# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **Unit commitment (three-bin formulation)**: full implementation of the
  Morales-España tight-and-compact UC model with status ($u$), startup ($v$),
  and shutdown ($w$) binary variables per committed generator.
  - Core constraints: logical transition (C1), generation bounds (C2),
    exclusion (C3).
  - Ramp constraints (C4–C5): tight state-dependent ramp-up/down limits
    with separate startup/shutdown ramp parameters.
  - Minimum up/down time (C6–C7): aggregated formulation at period level
    with correct handling of non-uniform block durations.
  - Hot/warm/cold startup cost tiers (C8–C10): time-dependent startup costs
    based on offline duration with validation and graceful fallback.
  - Piecewise linear heat rate curves: multi-segment fuel cost modeling with
    per-segment emission accounting and segment-bound constraints.
  - Commitment periods: optional coarser binary variable resolution to reduce
    integer variable count.
  - LP relaxation control: per-element (`relax` flag) and per-phase
    (`model_options.relaxed_phases`) with `PhaseRangeSet` expression parser
    supporting `"all"`, `"none"`, ranges, and comma-separated indices.
  - Reserve-UC integration: reserve headroom constraints become conditional
    on commitment status ($P_\max \cdot u$ instead of fixed $P_\max$).
  - Must-run mode: forces $u = 1$ for baseload or contractually committed units.
  - Chronological gating: UC only applies on stages with `chronological: true`;
    non-chronological stages dispatch normally (backward compatible).
  - New `Commitment` struct (`commitment.hpp`) with JSON serialization.
  - New `CommitmentLP` class (`commitment_lp.{hpp,cpp}`) with full output.
  - Comprehensive test suite: 40+ test cases in `test_commitment.cpp` (2600+
    lines) covering all constraint types, edge cases, and regression tests.
  - Documentation: `docs/unit-commitment.md` — dedicated guide with formulation,
    JSON reference, worked examples, and academic references.
- **Emission cost and cap framework**:
  - `generator.emission_factor` (tCO₂/MWh): per-generator CO₂ intensity.
  - `model_options.emission_cost` ($/tCO₂): system-wide carbon price, adds
    cost adder to dispatch and per-segment fuel costs.
  - `model_options.emission_cap` (tCO₂/year): per-stage CO₂ cap with
    endogenous carbon pricing via constraint dual variable.
- **Stage chronological flag**: `stage.chronological` boolean field enables
  block-sequential UC constraints on specific stages while leaving
  non-chronological stages for expansion planning.
- **SDDP cut types**: optimality and feasibility cuts are now distinguished
  in the SDDP solver, with named cut coefficients for better diagnostics.
- **Variable scaling**: `VariableScaleMap` support and decoupled `flow_scale`
  option for improved numerical conditioning of large hydro models.
- **Enum options framework**: solver options now use typed enums instead of
  raw strings, with compile-time validation.
- **Solver timeouts**: configurable time limits for LP solves; the solver
  abandons and reports partial results when the limit is exceeded.
- **Single-phase SDDP-to-monolithic fallback**: when the problem has only
  one phase, the SDDP solver automatically falls back to the monolithic
  solver instead of failing.
- **SDDP hot-start**: `sddp_hot_start` option to load cuts from a previous
  run and resume SDDP iterations.
- **plp2gtopt scale options**: `--auto-rsv-energy-scale` and `--auto-energy-scale`
  flags for automatic variable scaling during PLP conversion.
- **Hydro worked example**: Example 5 (simple hydro cascade) added to
  `PLANNING_GUIDE.md`.
- **CHANGELOG.md**: this file.

### Changed

- **Options refactor**: SDDP options no longer require the `sddp_` prefix
  (e.g., `max_iterations` instead of `sddp_max_iterations`). Old names are
  still accepted for backward compatibility.
- **MonolithicOptions**: monolithic solver options are now grouped under a
  dedicated `MonolithicOptions` struct for clarity.
- **`as_label` optimization**: label generation uses `std::to_chars` for
  faster integer-to-string conversion, measurably improving LP construction
  time on large cases.
- **SDDP cut I/O refactor**: extracted `sddp_cut_io` and `sddp_monitor`
  modules from `sddp_solver` for better separation of concerns.
- **SDDP aperture module**: extracted `sddp_aperture` for cleaner
  decomposition of the SDDP algorithm.

### Fixed

- **Exit codes**: the solver now returns correct exit codes (0=optimal,
  1=non-optimal, 2=input error, 3=internal error) instead of always
  returning 0.
- **Logging**: log messages no longer interleave in multi-scene SDDP solves;
  per-scene log context is now properly scoped.
- **`flat_map` portability**: replaced `std::flat_map` with `gtopt::flat_map`
  in `sddp_cut_io` to ensure compatibility across compilers.

### Testing

- **+160 unit tests**: coverage increased from 71.5% to 77.3%, with new
  tests for `sddp_cut_io`, `sddp_monitor`, battery expansion, reservoir
  efficiency, variable scaling, and enum options.
