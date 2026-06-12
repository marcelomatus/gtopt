# Irrigation agreements review (2026-06)

> Generated 2026-06-01 — deep critical review of the Maule/Laja
> irrigation agreements implementation, documentation, and
> bottom-up test ladder. Branch `feat/pmin-fcost-soft-floor`.
>
> Two-persona scope: (A) Hydro & electric modelling expert, (B) C++/LP
> implementation expert.

## TL;DR

**Doc gap.** The two largest design documents — `gtopt_vs_plp_comparison.md`
and `lp_column_row_audit.md` — were written when the agreement
expansion still lived in `scripts/plp2gtopt/{laja,maule}_writer.py`.
That logic has since moved to `scripts/gtopt_expand/` (3-stage
pipeline per `project_irrigation_pipeline.md`); the two writers in
`plp2gtopt/` are now thin shims (`scripts/plp2gtopt/laja_writer.py:1-17`,
`scripts/plp2gtopt/maule_writer.py:1-14`). The docs still attribute coverage
statements to the legacy writer names. There is **no document**
describing the new `gtopt_expand` tool, its 3-stage pipeline, or its
`.tampl/.tson/.md` templates (9321 lines of design surface).

**Code-vs-doc drift.** Three substantive drifts:

1. Laja economy accumulators are documented as `Not Yet Emitted by
   Writers` (`docs/analysis/irrigation_agreements/gtopt_vs_plp_comparison.md:1265-1267`,
   plus `lp_column_row_audit.md` §1.4), but they ARE emitted by
   `scripts/gtopt_expand/templates/laja.tson:470-520` (three
   VolumeRights with `purpose="economy"`, no `reset_month`, no
   `bound_rule` — a documented simplification at
   `templates/laja.tson:462-468`).
2. The `hydro_fail_cost` global default of 10 $/m³ from
   `project_irrigation_design.md §1` was never landed under that
   name; the field was renamed to `model_options.hydro_spill_cost`
   (`include/gtopt/planning_options_lp.hpp:230-244`). The 10 $/m³
   default is not consulted as a fallback for
   FlowRight/VolumeRight `fail_cost`.
3. La Invernada is described in `gtopt_vs_plp_comparison.md:1060` as
   "not emit[ting] the five balance variables, the conditional bound
   flipping, or the storage/bypass objective terms". The five
   FlowRights ARE now emitted by
   `scripts/gtopt_expand/templates/maule.tson:423-475` (P0 fix
   2026-04-11, captured in
   `.claude/agent-memory/irrigation-agreement-deep-review/la_invernada_template_p0.md`).
   The conditional bound-flipping is still not implemented, and
   `invernada_caudal_natural` is still a free variable rather than
   a data-pinned per-stage inflow (`maule.tson:441-446`).

**Top-3 P0 fixes before next production run.** None. Bumping to P1:
- **P1-a** Update `gtopt_vs_plp_comparison.md` §7.2/§7.4 and
  `lp_column_row_audit.md` to reflect that economy accumulators and
  La Invernada FlowRights are emitted.
- **P1-b** Pin `invernada_caudal_natural` from observed inflow
  schedule rather than keeping it as a free variable bounded only
  by `fmax`.
- **P1-c** Register the slack columns of `UserConstraintLP` in the
  AMPL variable map per design decision A4
  (`.claude/agent-memory/.../uc_slack_registration_gap.md`).

## Documentation audit

### `gtopt_vs_plp_comparison.md` (1596 lines)

**Persona A (modelling).** The PLP→gtopt entity table at §3-§4 is
the single best mapping document in the tree. Volume zone
derivations (`docs/analysis/irrigation_agreements/gtopt_vs_plp_comparison.md:553-583`)
match the code identity in `include/gtopt/right_bound_rule.hpp`
segment math; the Laja 4-zone / Maule 3-zone diagrams are accurate.
Resolución 105 monthly schedule (`gtopt_vs_plp_comparison.md:749-751`)
matches `seepage_and_colchones_analysis.md` §4. Coverage tables
§7.1-§7.4 are still correct for the **physics** but the §7.2
"Not yet emitted" list overlaps with what `gtopt_expand` actually
does emit.

**Persona B (implementation).** Every reference to
`laja_writer.py`/`maule_writer.py` is now a reference to a shim
that re-exports `gtopt_expand.{laja,maule}_agreement` (verified at
`scripts/plp2gtopt/laja_writer.py:12-13`). The document is
consistent within itself but does not mention `gtopt_expand`,
templates, or the 3-stage pipeline at all. §1.3 (lines 78-91)
should gain a §1.3b "Stage 2 — gtopt_expand" with pointers to
`scripts/gtopt_expand/{laja,maule}_agreement.py` (502 / 551 lines)
and the templates.

Specific issues:
- §3.1.4 Economy accumulators "Not Yet Emitted by Writer" (line
  428) is **wrong as of 2026-04**. (DRIFT P1.)
- §4.5 (line 1060) "The writer emits a single VolumeRight… but
  does not emit the five balance variables" — the five balance
  FlowRights exist now at `maule.tson:423-475`. (DRIFT P1.)
- §5.1 (line 1154) references "`use_cost` field on FlowRight (not
  yet in gtopt C++)". `FlowRight.fcost` and `FlowRight.uvalue`
  now exist (cf. `source/flow_right_lp.cpp:106-136` `resolve_bounds`).
  §9 roadmap (lines 1492-1503) still lists this as a missing C++
  feature. (DRIFT P2 — wording.)

### `plp_implementation.md` (575 lines)

**Persona A.** PLP-side narrative is faithful to the Fortran source
(matches `leelajam.f`, `parlajam.f`, `genpdlajam.f`/`genpdmaule.f`
on the canonical mirror). §1.6 LP constraint structure is the
clearest written description in the repo.

**Persona B.** Only 9 references to gtopt vocabulary in 575 lines.
§4.1/§4.2 "What Would Be Needed for gtopt" (line 500) is stale —
`bound_rule`, `reset_month`, `RightJunction`, `VolumeRight` all
exist.

### `right_junctions_analysis.md` (362 lines)

**Persona A.** Excellent catalog: 4 Maule + 3 Laja right junctions
at §3 (lines 305-322). Cross-checks cleanly against
`gtopt_vs_plp_comparison.md` §1.5/§1.6 diagrams.

**Persona B.** The summary table at §3 says Laja has 3 right
junctions; the current `laja.tson` uses ONE UserConstraint
(`laja_particion_derechos` at
`scripts/gtopt_expand/templates/laja.tson:545-549`) instead of
a `RightJunction` with `drain=false`. Mathematically equivalent;
the document does not flag the design choice. (DRIFT P2.)

### `lp_column_row_audit.md` (849 lines)

**Persona A.** §1 Laja column-by-column audit is correct on every
mandatory PLP variable. IQRS/IQPR/IQNR/IQER/IQSR are listed as
MISSING; per agent memory these are intentionally not modeled. The
audit would benefit from an "INTENTIONALLY NOT MODELED" state to
distinguish from real gaps.

**Persona B.** The audit names columns with prefix `frt_flow_*`,
`vrt_efin_*` matching the constexpr names on the LP classes
(`VolumeRightLP::ExtractionName`, `FlowRightLP::FailName`).
Verified by grep that `add_ampl_variable` call sites in
`source/volume_right_lp.cpp:257-269` and `flow_right_lp.cpp` use
only constexpr names — `feedback_no_magic_strings.md` discipline
is held.

### `seepage_and_colchones_analysis.md` (436 lines)

Pure PLP analysis; no gtopt code references. §4 La Invernada
(lines 370-398) is the only place describing IQIDN/IQISD semantics
in physical terms. No drift; could note the conditional bound flip
is not yet implemented in gtopt.

### `laja_agreement_research.md` (532), `maule_agreement_research.md` (340), `resolucion_105_and_updates.md` (407), `academic_and_presentations.md` (535)

Pure domain references; no gtopt drift to assess. Useful for the
next person editing the templates.

### Missing doc

There is **no architecture document** for `scripts/gtopt_expand/`.
The closest is the per-template `.md` file
(`scripts/gtopt_expand/templates/laja.md` 1039 lines, `maule.md`
1219 lines), which is template-rendered domain doc, not a pipeline
README. A 100-200 line
`docs/analysis/irrigation_agreements/gtopt_expand_pipeline.md`
explaining the pipeline, `cli.py` entry points, and the relation
to the `plp2gtopt` shims would close the documentation gap.

## Code-vs-doc drift

### P0 — none

The two "live bugs" cited in older review prompts (sentinel at
`volume_right_lp.cpp:89`, lower-bound update at
`flow_right_lp.cpp:298`) are both fixed; see
`source/volume_right_lp.cpp:95` (`LinearProblem::DblMax`)
and `source/flow_right_lp.cpp:106-136` (`resolve_bounds`).

### P1 — coverage misrepresentation

**P1-a.** Doc says Laja economy accumulators `IVESF/IVERF/IVAPF`
not emitted; they ARE emitted at
`scripts/gtopt_expand/templates/laja.tson:470-520`.

**P1-b.** `invernada_caudal_natural` is a free variable
`[0, qmax_invernada]` at
`scripts/gtopt_expand/templates/maule.tson:441-446`. Should be
data-driven from per-stage observed inflow. LP can pick any triple
`(deficit, sin_deficit, caudal_natural)` whose sum matches the
storage+bypass side, while PLP fixes `caudal_natural` to the
observed inflow.

**P1-c.** `UserConstraintLP` creates slack columns
(`include/gtopt/user_constraint_lp.hpp:61-67`,
`SlackName/SlackPosName/SlackNegName`) but
`source/user_constraint_lp.cpp` does not call
`sc.add_ampl_variable` for them. Decision A4 requires slack
visibility.

**P1-d.** `hydro_fail_cost` was renamed to `hydro_spill_cost`
(`include/gtopt/planning_options_lp.hpp:230`,
`source/gtopt_json_io_set.cpp:131`). No project-wide default of
10 $/m³ propagates to `FlowRight.fail_cost` / `VolumeRight.fail_cost`.
Templates carry their own literals; if a future template omits
`fail_cost`, soft constraints degenerate to zero penalty silently.

### P2 — wording / completeness

- `gtopt_vs_plp_comparison.md:1492-1503` lists `use_cost` as
  missing C++ feature; equivalent `fcost`/`uvalue` capability is in.
- Laja flow partition implemented as UserConstraint, not
  RightJunction; design choice undocumented.
- `lp_column_row_audit.md` needs `INTENTIONALLY-NOT-MODELED` state
  for IQRS/IQPR/IQNR/IQER/IQSR/IQLAJA.

## Test ladder review

Per `project_irrigation_test_ladder.md`. All file:line citations
verified against the current branch.

### Tier 1 — VolumeRight in isolation — **COMPLETE**

`test/source/test_volume_right_isolation.cpp` (572 lines), 7
TEST_CASEs covering Tier 1.1-1.7. Plus
`test_irrigation_reset_cross_phase.cpp` (652 lines) extends with
Tier 1.5b (5 subcases).

### Tier 2 — FlowRight in isolation — **COMPLETE**

- `test/source/test_flow_right.cpp` (1001 lines, 17 TEST_CASEs)
- `test_flow_right_modes.cpp` (1086 lines)
- `test_flow_right_substitution.cpp` (396 lines)
- `test_flow_right_unified_modes.cpp` (857 lines)

### Tier 3 — RightBoundRule edge cases — **COMPLETE**

`test/source/test_right_bound_rule.cpp` (645 lines, 19 TEST_CASEs),
including Laja 4-zone breakpoint continuity (L529),
volume-exactly-at-breakpoint (L589), `stage_month` axis (L348),
empty-segments returns `cap.value_or(0.0)` (L161).

### Tier 4 — VR + Reservoir balance — **COMPLETE**

`test/source/test_irrigation_coupling.cpp` Tier 4.1 (L774), 4.2
(L946), 4.3 (L1130), 4.4 (L1291).

### Tier 5 — FR + Turbine/Junction — **COMPLETE**

Same file, Tier 5.1 (L1470), 5.2 (L1631), 5.3 (L1799).

### Tier 6 — AMPL registration — **COMPLETE**

`test/source/test_irrigation_ampl_registry.cpp` (391 lines), Tier
6.1 (L96), 6.2 (L160), 6.3 (L238), 6.4 (L312).

### Tier 7 — UC + one right — **COMPLETE except 7.6**

`test/source/test_irrigation_user_constraints.cpp` Tier 7.1 (L164),
7.2 (L205), 7.3 (L242), 7.4 (L284, strict-mode error now default),
7.5 (L333). **Gap: UC over `flow_right.fail` not tested.**

### Tier 8 — UC with multiple rights — **COMPLETE**

Same file, Tier 8.1 (L386), 8.2 (L457), 8.3 (L503), 8.4 (L558),
8.5 (L607), 8.6 (L677, P0 defender). Plus
`test_irrigation_invernada_boundary.cpp` (653 lines, 6 TEST_CASEs
covering Tier 8.6b).

### Tier 9 — Maule full agreement — **COMPLETE (scaled-down)**

`test/source/test_irrigation_maule_agreement.cpp` Tier 9.1 (L271),
9.2 (L405), 9.3 (L476). **Gap (doc test): Resolución 105
enforcement (Tier 9.4) not present; ladder says "will fail today".**

### Tier 10 — Laja full agreement — **COMPLETE**

`test/source/test_irrigation_laja_agreement.cpp` Tier 10.1 (L267),
10.2 (L401), 10.3 (L478). Doc tests still pending for filtración,
Salto del Laja waterfall, forced_flows, manual_withdrawals (all
pampl-visible by design per `stage2_silent_drops.md`).

### Tier 11 — Maule + Laja crosstalk — **COMPLETE except 11.4**

`test/source/test_irrigation_maule_laja_crosstalk.cpp` Tier 11.1
(L331), 11.2 (L362), 11.3 (L434). **Gap: SDDP-with-cut-store
regression (Tier 11.4) not present.**

### Tier 12 — Python round-trip — **COMPLETE**

`scripts/plp2gtopt/tests/test_tier12_round_trip.py` covers every
Stage-2 pampl-visible field. `scripts/gtopt_expand/tests/test_round_trip.py`
covers the Stage-2 tool's output.

### Overall ladder coverage

18 test files, 22,284 lines (vs 14,932 in the 2026-04 baseline —
50% growth driven by Tier 8.6b boundary suite and Tier 9/10
agreement integrations). All 12 tiers covered. Three documented
gaps, all P2.

## Bottom-up test plan

Most layers are covered. Below lists ONLY the missing or weak
defender tests. No duplicates with existing tests.

### L0 — Pure data structures

**L0.1 — Half-open-interval semantics on RightBoundRule breakpoints.**
- File: `test/source/test_right_bound_rule.cpp` (extend existing
  TEST_CASE at L529).
- SUBCASE: `"evaluate(V_exact) == evaluate(V_exact + ε)"` —
  current test covers `V_i - ε ≈ V_i + ε`; the boundary semantics
  rule is unguarded.
- Primitive: `find_active_bound_segment` at
  `include/gtopt/right_bound_rule.hpp`.
- Defends: half-open-interval rounding convention.

**L0.2 — `state(var)` Phase-1e wrapper.**
- File: `test/source/test_pampl_parser.cpp` (extend, 611 lines).
- TEST_CASE: `"state(var) wrapper sets state_wrapped flag without
  resolver assertion"`.
- Primitive: `include/gtopt/ampl_variable.hpp` `AmplVariableKind`
  enum.
- Defends: pre-Phase-2 transparent passthrough.

### L2 — Two primitives coupled

**L2.1 — Per-cell mutex two-thread harness.**
- File: `test/source/test_irrigation_ampl_registry.cpp` (extend).
- TEST_CASE: `"Tier 6.5 - concurrent add_to_lp per (scene, phase)
  does not duplicate registrations"`.
- Pattern: two threads each call `add_to_lp` on distinct
  `(scenario, stage)` cells; verify no double registration.
- Primitive: per-`(scene, phase)` mutex in `source/system_context.cpp`.
- Defends: `feedback_per_cell_mutex.md` invariant.

### L3 — UserConstraints added one at a time

**L3.1 — UC over `flow_right.fail`.**
- File: `test/source/test_irrigation_user_constraints.cpp` (extend).
- TEST_CASE: `"Tier 7.6 - UserConstraint over flow_right.fail
  attribute resolves and binds"`.
- Fixture: one FlowRight with `fail_cost > 0`; one UC with
  `flow_right('fr1').fail <= 1.0`.
- Assertions: resolver does not throw; LP feasible; UC binds when
  demand > 1.0.
- Defends: AMPL-side exposure of conditionally-created columns.

**L3.2 — UC slack visibility in AMPL registry (doc test).**
- File: `test/source/test_user_constraint_advanced.cpp` (extend).
- TEST_CASE: `"UC with penalty sugar creates AMPL-visible slack"`.
- Fixture: one UC `flow_right('x').flow <= 100 : penalty=1000`.
  Query AMPL variable map for
  `(class="UserConstraint", attr="slack", uid=...)`.
- Assertions: lookup returns slack column index; reporter can find
  it post-solve.
- Defends: `uc_slack_registration_gap.md` P1.
- **Expected to FAIL today** until registration is wired up.

### L4 — Full agreement

**L4.1 — Resolución 105 ecological flow (doc test).**
- File: `test/source/test_irrigation_maule_agreement.cpp` (extend).
- TEST_CASE: `"Tier 9.4 - Resolución 105 ecological flow enforced
  (doc test)"`.
- Fixture: 12-stage Maule with monthly Res105 demand from
  `QRiego105(12)` (per `gtopt_vs_plp_comparison.md:749-751`).
- Assertions: per-block discharge >= monthly Res105 (modulo
  slack); LP feasible; primal matches demand schedule.
- **Expected to FAIL** until ecological flow wiring lands.

**L4.2 — Laja economy accumulators binding.**
- File: `test/source/test_irrigation_laja_agreement.cpp` (extend).
- TEST_CASE: `"Tier 10.4 - economy accumulators IVESF/IVERF/IVAPF
  drain when irrigation rights are spent"`.
- Fixture: Laja LP with positive `saving_rate` on the three
  economy VRs, deterministic inflows producing ~50 hm³ over a
  12-stage horizon.
- Assertions: economy VR final volume > eini at end; no
  reset_month side effects.
- Defends: P1-a coverage drift — accumulators are emitted and
  binding.

### L5 — Cross-talk and SDDP

**L5.1 — SDDP single-aperture with both agreements.**
- File: `test/source/test_irrigation_maule_laja_crosstalk.cpp`
  (extend).
- TEST_CASE: `"Tier 11.4 - SDDP single-aperture forward+backward
  with both agreements + cut store"`.
- Fixture: 2-phase SDDP, single aperture per phase, both
  Maule+Laja, deterministic inflows, `cut_sharing = none` per
  `feedback_cut_sharing_unsafe.md`.
- Assertions: finite cut from backward; forward uses cut for LB;
  `efin[N] == eini[N+1]` non-reset boundaries; `eini[reset]`
  clamped by `skip_state_link`.
- Defends: regression for `6a87a8b9`.

### L6 — Python round-trip

Covered by Tier 12. No additions.

## Methodology

### Files read

Production code:
- `include/gtopt/volume_right_lp.hpp`
- `include/gtopt/flow_right_lp.hpp`
- `include/gtopt/right_bound_rule.hpp`
- `include/gtopt/user_constraint_lp.hpp`
- `include/gtopt/ampl_variable.hpp`
- `include/gtopt/pampl_parser.hpp`
- `include/gtopt/planning_options.hpp`
- `include/gtopt/planning_options_lp.hpp`
- `source/volume_right_lp.cpp` (full, 435 lines)
- `source/flow_right_lp.cpp` (head + dispatch path)
- `source/user_constraint_lp.cpp` (header + slack creation surface)

Templates / Python:
- `scripts/gtopt_expand/templates/laja.tson` (551 lines)
- `scripts/gtopt_expand/templates/maule.tson` (779 lines)
- `scripts/gtopt_expand/laja_agreement.py` (551 lines)
- `scripts/gtopt_expand/maule_agreement.py` (502 lines)
- `scripts/plp2gtopt/laja_writer.py` (shim, 17 lines)
- `scripts/plp2gtopt/maule_writer.py` (shim, 14 lines)

Docs read in full or in section:
- All 9 `docs/analysis/irrigation_agreements/*.md` files
  (5632 lines)

Tests audited (18 files, 22,284 lines total).

### Patterns checked

- Magic-string discipline (`feedback_no_magic_strings.md`):
  `add_ampl_variable` call sites in `volume_right_lp.cpp:257-269`
  and `flow_right_lp.cpp` use constexpr name constants only.
- `std::optional<double>` (`feedback_no_nan.md`):
  `volume_right_lp.cpp:121,148` use `.value_or(LinearProblem::DblMax)`
  and `.value_or(0.0)`; no NaN initialisers.
- `DblMax`/infinity (`feedback_infinity_scaling.md`):
  `volume_right_lp.cpp:95-126` uses `LinearProblem::DblMax`.
- Stage-month fail-fast: visible at `volume_right_lp.cpp:176-181`
  via `require_stage_month(...)`.

### Patterns NOT checked

- Complete grep for latent `Real::max()` sentinel use across
  `source/*.cpp`.
- `defer_state_link` infrastructure live-vs-dead status.
- Concurrency under `aperture_chunk_size > 1` of irrigation
  `add_to_lp`.
- `gtopt_check_pampl` validation of generated `.pampl` files from
  `gtopt_expand`.
- Solver-side primal/dual values of L3-L5 proposals — read-only
  review.

---

**IRRIGATION-AGREEMENT REVIEW VERDICT: MINOR**

Zero P0. Four P1 drifts. All in documentation/coverage staleness
or A4 visibility, not in dispatch correctness. The Tier 1-12 test
ladder is intact and grown 50% since the 2026-04 baseline.
