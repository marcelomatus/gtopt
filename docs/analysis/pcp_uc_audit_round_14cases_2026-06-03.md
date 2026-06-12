# PCP uc_audit round — all 14 support/plexos daily cases

**Date.** 2026-06-03  **Scope.** Run `uc_audit --strict` on every
`support/plexos/pcp_*` bundle and drive the B2/B5 buckets to empty.

## Pipeline (per case)

Driver: [`support/plexos/run_uc_audit_round.py`](../../support/plexos/run_uc_audit_round.py).

1. Extract the sibling `RES<date>.zip.xz` `.accdb`, `mdb-export` the four
   solution tables `uc_audit` reads (`t_object`, `t_membership`, `t_key`,
   `t_data_0`) into a plain-CSV `plexos_cache_plain/`.
2. `plexos2gtopt <DATOS> -o <out> --no-check` → planning JSON + `uc_*.pampl`.
3. `gtopt <json> --no-scale --lp-only -l <stem>` (LP assembled, **not solved**;
   run from `<out>` because the JSON references the PAMPL files by basename).
4. `uc_audit --plexos-cache <cache> --gtopt-dir <out> --strict`.

LP generation requires a gtopt binary built from this branch — the installed
May-29 binary rejects the converter's `loss_cost_eps` model option.

## Result

**14 / 14 PASS** (`--strict`: B2 RHS-mismatch = 0, B5 hard-soft = 0 everywhere).
Every case also converts (conv = 0) and assembles its LP cleanly (lp = 0).
B6 (soft-in-PLEXOS / hard-in-gtopt, 0–8 per case) is informational and does
not gate `--strict`.

## First-round failures (3 cases) — all B2, all false positives

| Case | B2 item | gtopt | PLEXOS | Verdict |
|---|---|---|---|---|
| 2025-10-05, 2025-10-19 | `PANGUEpriority` (`>=`) | −10000 | 20 (const) | never binds (price 0) |
| 2025-12-21 | `SD_2025128684_Guacolda_Maitencillo` ×2 (`<=`) | 400 | rhs_min 400, rhs_max 10000 | gtopt correct; 10000 = no-limit sentinel |

A direction-aware first patch (min for `<=`, max for `>=`) then surfaced
`Reg_SouthZone` on 5 *previously-passing* cases — a binding `<=` reserve UC
whose gtopt RHS is a block profile spanning **[187.42, 320]** vs PLEXOS
**[207, 320]**. The ranges overlap; the 187.42-vs-207 "mismatch" is two
**non-aligned blocks**, an artifact of comparing a global-min to a global-min.

## Root cause — `uc_audit` B2 had two defects (not a converter bug)

Both gtopt and PLEXOS carry block-varying RHS, so any single-aggregate
comparison (max-vs-max **or** min-vs-min) is unsound: the extrema fall in
different blocks. The fix (`scripts/plexos2gtopt/uc_audit.py`):

1. **Range-overlap test.** Flag only when gtopt's value range `[min, max]` is
   *disjoint* from PLEXOS's `[rhs_min, rhs_max]` (gap / scale > 5 %). This
   catches true unit/sign/scale errors (e.g. ANTUCO 137 MW vs 83.3 m³/s) while
   dissolving the ±10000 / ±100000 "contingency-off" sentinels (gtopt's real
   400 MW cap sits inside PLEXOS's [400, 10000] band) and overlapping block
   profiles (`Reg_SouthZone`).
2. **"PLEXOS actually binds" guard** (`price_sum_abs > 0 and
   hours_binding_sum > 0`) — mirrors the existing B6 guard and the audit's
   stated goal of silencing never-binding structural noise. Drops
   `PANGUEpriority` (gtopt's `_horizon_value(prefer_min=True)` picks a relaxed
   `>= -10000` floor row on the Oct horizon while PLEXOS holds it at 20, but
   never binds it — immaterial to the solution).

Tests: `test_b2_flags_true_scale_mismatch`,
`test_b2_suppresses_le_nolimit_sentinel`,
`test_b2_suppresses_overlapping_block_profile`,
`test_b2_suppresses_never_binding` in `tests/test_uc_audit.py`.

## Noted for follow-up (not blocking, zero solution impact)

- `PANGUEpriority` RHS is date-windowed in gtopt (−10000 for Oct-2025 runs,
  20 for Nov–Dec). PLEXOS uses 20 consistently but never binds it. If exact
  RHS fidelity is wanted, revisit `_horizon_value(prefer_min=True)` for `>=`
  rows; today it is immaterial.
- The round's 04-22 sub-convert hit a transient
  `build_planning() … 'line_losses_mode'` `TypeError`; a fresh standalone
  convert succeeds (the converter source was being edited during the
  background round). Regenerated 04-22 end-to-end: conv = 0, lp = 0, PASS.
