# TODO

Tracked in [GitHub Issues](https://github.com/marcelomatus/gtopt/issues).

## Hydro / Irrigation

- [ ] Revisar el uso de caudales promedios en el Maule ([#347](https://github.com/marcelomatus/gtopt/issues/347))
- [ ] Revisar los limites de caudales diarios, mensuales ([#348](https://github.com/marcelomatus/gtopt/issues/348))

## Losses

- [ ] Revisar el modelo de perdidas por uno mas liviano (3x PLP) ([#350](https://github.com/marcelomatus/gtopt/issues/350))

## Performance

- [ ] Replace memory malloc with suggested alternative ([#321](https://github.com/marcelomatus/gtopt/issues/321))

## SDDP Elastic Filter

- [ ] **Per-variable elastic penalty cost for non-reservoir state variables** —
  Reservoir state variables now support per-element `scost` (via
  `Reservoir::scost` or `state_fail_cost × mean_production_factor`).
  Extend the same mechanism to other state variable types (Battery,
  CapacityObject) so each element class can define its own penalty cost
  in its physical units.

## Hydro / Test Cases

- [ ] **Reservoir with two turbines** — Add a test case (e.g. Colbún-like)
  with a single reservoir connected to two turbines, each with its own
  `ReservoirProductionFactor` curve and potentially different `efficiency`
  values.

## Numerical Conditioning

- [ ] **Ruiz scaling (geometric mean iterative scaling)** ([#352](https://github.com/marcelomatus/gtopt/issues/352)) — Implement Ruiz
  equilibration as a pre-processing pass in `lp_build()`.  Ruiz scaling
  alternately normalizes rows and columns by their infinity-norm until
  convergence (typically 5–10 iterations).  This produces a better-conditioned
  matrix than single-pass row equilibration alone, especially for problems with
  heterogeneous variable scales.  Reference: Ruiz, D. (2001) "A scaling
  algorithm to equilibrate both rows and columns norms in matrices".
  Implementation notes:
  - Add as `LpBuildOptions::ruiz_scaling` (bool, default false).
  - Iterate: for each row, compute max |a_ij|; for each col, compute max |a_ij|.
    Scale rows and columns by 1/sqrt(max).  Repeat until max change < tolerance.
  - Update `col_scales` and `row_scales` accordingly so dual/primal unscaling
    remains correct.
  - Must run *after* the existing row equilibration pass (or replace it).

## Completed

- [x] Revisar el pampl, como se usa lo de los meses ([#349](https://github.com/marcelomatus/gtopt/issues/349))
- [x] Verificar que no hay ninguna escala de col manual ([#351](https://github.com/marcelomatus/gtopt/issues/351))
- [x] Verify all scripts and docs ([#322](https://github.com/marcelomatus/gtopt/issues/322))
- [x] `run_gtopt` should not print output by default — add quiet mode ([#323](https://github.com/marcelomatus/gtopt/issues/323))
- [x] Minimal solver check interface ([#324](https://github.com/marcelomatus/gtopt/issues/324))
- [x] Check loading of state variables ([#325](https://github.com/marcelomatus/gtopt/issues/325)) — test suite added 2026-04-01
- [x] Check stage/scenario setting with phase `eini`/`efin` ([#326](https://github.com/marcelomatus/gtopt/issues/326)) — fixed 2026-03-16
- [x] Refactor named enum ([#327](https://github.com/marcelomatus/gtopt/issues/327)) — `NamedEnum` concept in `enum_option.hpp`
