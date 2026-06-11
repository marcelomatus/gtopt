# `--lp-debug` writes two identical `.lp` files in monolithic mode

## Reproduction

Run command:

```
gtopt -s DATOS20260422.json --threads 20 --no-mip --no-scale --lp-debug \
  --log-directory /tmp/gtopt-pcp-fcr24/lp_debug \
  --lp-compression none --set solver_options.log_mode=detailed
```

Outputs (both 227,818,089 bytes, MD5 `cb93ff8aef79b2f41725e6604134496d`):

- `monolithic.lp`                       — mtime `1779416012`
- `gtopt_lp_scene_0_phase_0.lp`         — mtime `1779416018` (6 s later)

`cmp` reports byte-identical. First/last lines, header (`\Problem name:
gtopt_0_0`), objective row, and every coefficient match.

## Call sites

### File 1 — `monolithic.lp`

`source/gtopt_lp_runner.cpp:793-806` (function `gtopt_lp_runner_main`,
after `build_all_lps_eagerly()`, before `run_solver()`):

```cpp
const bool monolithic_lp_debug = opts.lp_debug.value_or(false)
    && planning_lp.options().method_type_enum() == MethodType::monolithic
    && !opts.lp_file.has_value();
...
} else if (monolithic_lp_debug) {
  const auto lp_path =
      (std::filesystem::path(log_dir) / "monolithic.lp").string();
  planning_lp.write_lp(lp_path);       // → PlanningLP::write_lp
}
```

`PlanningLP::write_lp` (`source/planning_lp.cpp:1413-1447`) detects that
there is exactly one cell in monolithic mode (`n_cells == 1`) and falls
through to `system.linear_interface().write_lp(stem)` *without* the
`_scene_X_phase_Y` decoration — hence the bare filename `monolithic.lp`.

### File 2 — `gtopt_lp_scene_0_phase_0.lp`

`source/monolithic_method.cpp:102-128` (function `MonolithicMethod::solve`,
run from `PlanningLP::resolve` → `run_solver` in `gtopt_lp_runner.cpp:592`):

```cpp
LpDebugWriter lp_writer(lp_debug ? lp_debug_directory : std::string{}, ...);
if (lp_writer.is_active()) {
  const auto lp_stem =
      (std::filesystem::path(lp_debug_directory) / "gtopt_lp").string();
  for (const auto& phase_systems : planning_lp.systems()) {
    for (const auto& system : phase_systems) {
      ...
      if (auto lp_result = system.write_lp(lp_stem)) { ... }
    }
  }
}
```

`SystemLP::write_lp` (`source/system_lp.cpp:1474-1488`) decorates the
stem unconditionally via
`as_label(stem, "scene", scene().uid(), "phase", phase().uid())`, hence
`gtopt_lp_scene_0_phase_0.lp` even for a single-cell run.

## Are they identical?

Yes — **byte-identical** (md5 match). Both are written at the same point
in the LP lifecycle: after `flatten()` (which is where equilibration /
col-scales would have been applied if enabled), but before
`LinearInterface::resolve()`. With `--no-scale` the
`equilibration_method` resolves to `none` and `col_scales` is empty
(`source/system_lp.cpp:1037`), so no transformation occurs between the
two writes anyway. Boundary cuts could in principle differ the second
file (lines 47-86 of `monolithic_method.cpp` run *before* the second
write), but this run has no `monolithic_boundary_cuts_file` set, so the
LP is untouched.

## Stage represented

Both files capture the **same LP**: post-flatten, post-equilibration (a
no-op under `--no-scale`), pre-presolve, pre-solve, physical
coefficients (since `scale_objective` divides into the LP coefficients
during flatten and `--no-scale` does not disable that, both files
reflect identical `scale_objective` handling — they are not distinct
"physical vs scaled" views).

## Recommendation

`monolithic.lp` (the runner-level dump) is **redundant**. It exists as
backstop logic added by `gtopt_lp_runner.cpp:787-806` ("`--lp-debug` in
monolithic mode used to be a silent no-op…"), but
`MonolithicMethod::solve` now performs the same dump via the standard
`LpDebugWriter` path, honoring `lp_debug_compression` and the
scene/phase range filters (the runner path ignores all four
`lp_debug_*_min/max` knobs).

Preferred fix: drop the `monolithic_lp_debug` branch in
`gtopt_lp_runner.cpp:793-806`, keep `MonolithicMethod`'s
`LpDebugWriter` path as the single source of truth, and document that
`--lp-debug` always emits the `gtopt_lp_scene_{S}_phase_{P}.lp` naming
(consistent with SDDP / cascade). The runner path is only needed for
`--lp-only` (which exits before `run_solver`) — guard that case
explicitly if it must keep working.

Lower-risk alternative: keep the runner path only when `--lp-only` is
set (skip-solver path), and let `MonolithicMethod` own the dump
otherwise. Either way, eliminate the duplicate 227 MB write.
