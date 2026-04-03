# TODO

Tracked in [GitHub Issues](https://github.com/marcelomatus/gtopt/issues).

## Hydro / Irrigation

- [ ] Revisar el uso de caudales promedios en el Maule
- [ ] Revisar los limites de caudales diarios, mensuales
- [ ] Revisar el pampl, como se usa lo de los meses

## Losses

- [ ] Revisar el modelo de perdidas por uno mas liviano (3x PLP)

## LP Build

- [ ] Verificar que no hay ninguna escala de col manual

## Performance

- [ ] Replace memory malloc with suggested alternative ([#321](https://github.com/marcelomatus/gtopt/issues/321))

## Scripts & Documentation

- [ ] Verify all scripts and docs ([#322](https://github.com/marcelomatus/gtopt/issues/322))
- [ ] `run_gtopt` should not print output by default — add quiet mode ([#323](https://github.com/marcelomatus/gtopt/issues/323))

## Solver

- [ ] Minimal solver check interface ([#324](https://github.com/marcelomatus/gtopt/issues/324))

## Numerical Conditioning

- [ ] **Ruiz scaling (geometric mean iterative scaling)** — Implement Ruiz
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

- [x] Check loading of state variables ([#325](https://github.com/marcelomatus/gtopt/issues/325)) — test suite added 2026-04-01
- [x] Check stage/scenario setting with phase `eini`/`efin` ([#326](https://github.com/marcelomatus/gtopt/issues/326)) — fixed 2026-03-16
- [x] Refactor named enum ([#327](https://github.com/marcelomatus/gtopt/issues/327)) — `NamedEnum` concept in `enum_option.hpp`
