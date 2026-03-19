# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

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
- **plp2gtopt scale options**: `--auto-vol-scale` and `--auto-energy-scale`
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
