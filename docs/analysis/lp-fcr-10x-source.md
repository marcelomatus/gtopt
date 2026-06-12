# Reservoir flow_conversion_rate 10× inflation between JSON and LP

**Date**: 2026-05-21
**Repro**: `/tmp/gtopt-pcp-fcr24/`
**Binary**: `/tmp/gtopt-build-53wv/standalone/gtopt` (built 2026-05-21 21:27, AFTER
the `90315e17b` fallback fix)

## Confirmed observations

1. Bundle JSON (`DATOS20260422.json`, 22:39:15) ships every Reservoir with
   `"flow_conversion_rate": 0.041666666666666664` (= 1/24, the CMD-units
   conversion emitted by the *uncommitted* `scripts/plexos2gtopt/gtopt_writer.py`).
2. LP file (`lp_debug/gtopt_lp_scene_0_phase_0.lp`, 22:39:36) shows the
   energy-balance coefficient `(fcr/fout_eff) × block.duration() × dc_stage_scale`
   evaluating to **exactly 10× the expected value**:
   - 1 h block → 0.41667 (expected 0.04167)
   - 2 h block → 0.83333 (expected 0.08333)
   - 5 h block → 2.08333 (expected 0.20833)
   - 6 h block → 2.50000 (expected 0.25000)
   - 8 h block → 3.33333 (expected 0.33333)
   - Ratio LP-coef / (1/24 × duration) = 10.0 exactly for every block.
3. Running the same binary with `--json-file merged.json` produces a merged
   Planning whose ONLY mutated field is `flow_conversion_rate`:
   `0.41666666666666663` (10×). Every other Reservoir field
   (`eini`, `emin`, `emax`, `efin`, `efin_cost`) round-trips byte-identically.
4. `--no-scale` is in effect (`scale_objective=1`, `scale_theta=1`,
   `equilibration=none`, `auto_scale=false` per the gtopt_1.log header).
   `auto_scale_reservoirs` is gated off, so no per-element variable_scales
   entry is injected. `Reservoir.extraction` column gets `col.scale = 1.0`
   from the empty VariableScaleMap lookup.

## Ruled out

| Hypothesis | Verdict |
|---|---|
| `value_or(default_flow_conversion_rate)` fallback firing because parse drops the field | NO — direct `daw::json::from_json<Reservoir>` and `from_json<TinySys>` probes (`/tmp/fcr_probe.cpp`, `/tmp/fcr_probe4.cpp`) parse `0.04167` from the SAME bundle line and round-trip it as `0.04167`. JSON binding is correct. |
| `block.duration()` carrying a 10× factor | NO — junction-balance rows show extraction with coefficient 1.0; only the storage energy-balance row is affected. |
| `dc_stage_scale` (daily-cycle scaling) | NO — would need `daily_cycle = true` AND `stage.duration() > 24 h`, AND it produces `24/168 = 0.143` (sub-unit), not 10×. Bundle has no `daily_cycle` field. |
| Column scale from `VariableScaleMap` | NO — `--no-scale` empties the map (only Bus.theta@1.0 injected); `lookup("Reservoir", "extraction", uid)` returns 1.0. |
| Equilibration | NO — `equilibration_method = none` under `--no-scale`. |
| `Reservoir::default_flow_conversion_rate` (struct in-class init = 0.0036) | NO — `value_or` fallback only fires when JSON omits the field. Bundle sets it explicitly. |
| The previous `value_or(3.6)` literal in `reservoir_lp.hpp` (fixed in 90315e17b) | NO — binary post-dates the fix; JSON has explicit value anyway. |
| `expand_reservoir_constraints` / `System::merge` / any in-tree mutator of `reservoir.flow_conversion_rate` | NO — full repo grep (`source/`, `include/`) finds zero assignment sites except `json_volume_right.hpp:74` (irrelevant) and the in-class init. |
| `canonicalize_json_keys` / `decanonicalize_json_keys` | NO — only rewrites object keys, never touches numeric values. |
| `apply_set_options` / `apply_cli_options` / `validate_planning` | NO — none touch `reservoir.flow_conversion_rate`. |

## Where the 10× actually lives

**Unknown.** Direct daw::json round-trip of the same bundle text via
`from_json<Reservoir>` and `from_json<TinySys>` (StrictParsePolicy) yields
`0.041666666666666657`. But `gtopt --json-file merged.json` on the same
bundle yields `0.41666666666666663` for every Reservoir while preserving
all other fields.

Smallest reproducer not yet found, but the multiplication MUST happen
between `from_json<Planning>` (inside `parse_planning_files`) and
`write_json_output` (inside `gtopt_main` ~line 530). The only steps
between are:

1. `parse_planning_files` → `canonicalize_json_keys` (no value mutation)
   → `from_json<Planning>(…, StrictParsePolicy)` → `my_planning.merge(plan)`
   (System::merge → `gtopt::merge` on vectors, no field mutation)
2. `configure_planning`: `apply_set_options` (only `solver_options.log_mode`
   was passed) → `apply_cli_options` → `load_user_constraints` →
   `validate_planning` (const access).

None of these has a visible 10× site on `flow_conversion_rate`.

## Hypotheses worth probing next

1. **Hidden global hook**. Check `Planning` / `System` move-ctor or
   copy-ctor for any non-default behaviour. Both should be defaulted; a
   typo (e.g. `flow_conversion_rate *= 10` accidentally added) would be
   obvious by grep but maybe the multiplier hides as a *bitcast* or via
   `std::transform` on the array. A binary `strings` on `libgtopt.a` for
   the exact double bit pattern `7b 14 ae 47 e1 7a c4 3f` (= 0.04167)
   coupled with the disassembly might surface it.

2. **A daw::json TU under unity-build accidentally aliased `OptReal`
   parsing**. The repo unity-builds tests but NOT production
   (verify in CMakeCache). If a TU were unity-built and one TU
   intermediates fcr by 10× via a member adapter, the production parse
   could miscompile. Low probability — the `from_json<TinySys>` probe
   would also hit this.

3. **A different binary**. The user's claim is that the LP and merged
   JSON came from `/tmp/gtopt-build-53wv/standalone/gtopt`. Confirm via
   `--version` AND a fresh build-from-scratch from current HEAD. If a
   fresh build no longer shows the 10×, the existing scratch build was
   built from a stale source snapshot (Ninja can miss dependency edges
   on `_deps`/CPM regenerated headers).

4. **An overlay JSON injected silently**. Search for any default-loaded
   "fcr scaling" overlay (`SHARE_DIR/gtopt/*.json`) that might patch
   reservoirs at parse time. None found in `share/gtopt/` but worth
   triple-checking the `unit_dialects.json` / `naming_dialects.json`
   loader path for value-rewriting branches.

## Concrete next step (recommended)

Rebuild gtopt fresh from current HEAD and re-run the same command:

```bash
BUILD_DIR=$(bash /home/marce/git/gtopt/tools/mk_scratch_build.sh)
cmake -S /home/marce/git/gtopt/all -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=CIFast -DCMAKE_CXX_COMPILER=g++-15 \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build "$BUILD_DIR" -j"$(nproc)"
cd /tmp/gtopt-pcp-fcr24
"$BUILD_DIR/standalone/gtopt" -s DATOS20260422.json \
  --json-file merged_fresh.json --no-mip --no-scale --lp-only
grep -oE '"flow_conversion_rate":[0-9.]+' merged_fresh.json | head -3
```

If the fresh binary outputs `0.041666...`, the existing
`/tmp/gtopt-build-53wv` was built from stale headers (likely
`reservoir_lp.hpp`'s `3.6` literal pre-90315e17b, plus a now-removed
unit-conversion step somewhere) and the 10× is a stale-build artifact.
If the fresh binary STILL outputs `0.4166...`, the bug is live in HEAD
and needs an instrumented `from_json<Planning>` to localise.

## Why "10× = 10/24" matters for diagnosis

10 is a very specific factor — not 1000 (the old `3.6` vs `0.0036`
mismatch), not 24 (CMD ↔ hm³), not 86400, not the line count, not any
power of 10 / day count combination from the bundle metadata. The most
suspicious interpretation: somewhere the value got converted as if from
"% of daily flow" to "per-block fraction", which is a 10× factor when
n_blocks ≈ 10 per day. **The bundle has 111 blocks over 168 h ≈ 6.6
blocks/day, not 10, so this is also not a clean match.**

The cleanest match is `fcr_lp = fcr_json × 10` with no day/block
dependence — which points at an unconditional `*= 10` somewhere, or a
unit-conversion hook that fires once per Reservoir at a step I have
not yet identified.
