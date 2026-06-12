# SDDP Cut-Sharing Fix Plan — 2026-04-30

Plan to remediate the cut-sharing LB-overshoot bug diagnosed and
audited 2026-04-30. Background: `lp-numerics-expert` deep audit
established that the justifying comments at
`source/sddp_cut_sharing.cpp:88-92 / 134-139 / 189-197` are
mathematically wrong; gtopt implements **multi-cut SDDP**
(per-scene α columns), which by construction does not permit
cross-scene cut broadcasting except when scenes share the
*literal* same sample path. Production runs like juan/gtopt_iplp
have 16 scenes, all structurally similar (same network/costs/
demand) but each drawing a distinct hydrology realization — and
that distinct-realization condition alone is enough to break cut
sharing. Currently shipped: juan json patched to
`cut_sharing_mode: none`. Synthetic test
`test/source/test_sddp_bounds_sanity.cpp` pins the regression as
WARN-only.

This plan layers a P0 → P1 → P2 fix sequence, each independently
shippable. Land in order.

**Status as of 2026-04-30 evening**: Phase 0 and Phase 1 are SHIPPED.
Phase 2 was DROPPED after analysis (only fixes a degenerate
synthetic-test case with no production utility). Phase 3 is
DEFERRED until benchmarks justify a single-cut SDDP rewrite.

---

## Goal & Non-Goals

**Goal**: make non-`none` cut-sharing modes either mathematically
correct OR clearly forbidden, so no production run can silently
produce LB > UB.

**Terminology**: the validity question is *not* about scenes being
"similar" or "different" in a colloquial sense (juan's scenes are
all structurally similar — same network/costs/demand, only the
inflow time series differs). The relevant condition is whether
**`Q_s(x) = Q_d(x)` for every reachable state x** — i.e., whether
two scenes are literally the same random realization. This is
typically true only for synthetic test fixtures that deliberately
duplicate inflow schedules, and is essentially never true for
production multi-scenario runs. In this plan we use **"distinct-Q
scenes"** to mean any pair of scenes whose value functions could
differ (the practical default), and **"identical-Q scenes"** for
the deliberately constructed equal-sample-path case.

**Non-goals**:
- Removing the `accumulate` / `expected` / `max` enums (kept for
  research / homogeneous-scenarios experiments).
- Re-architecting gtopt's per-scene-LP topology (P2 explores a
  surgical alternative; full monolithic-α rewrite is out of scope
  for this plan).

---

## Architecture refresher

gtopt implements **multi-cut SDDP** (Pereira-Pinto 1991): each
(scene, phase) cell has its own LP with its own α^k_s column.
Each scene s is a single sample path of the underlying stochastic
process, and α^k_s bounds **scene s's value function along scene
s's specific sample path** — `Q_s(x)`, NOT `E[Q(x)]`.

LP cost coefs carry `cost_factor_s = prob_s · discount · duration`
via `block_ecost` (`cost_helper.hpp:115-121`). A scene-S cut built
by `build_benders_cut_physical` has every coefficient and the RHS
in `prob_S`-weighted units relative to `Q_S(·)`.

Broadcasting that row to scene D forces `α^k_D ≥ prob_S · Q_S*(x_S)`,
which is a valid bound on `α^k_D` (i.e. on `Q_D(·)`) only when:
1. `prob_S = prob_D` (probability mismatch handled by the rescale
   in Phase 2), AND
2. `Q_S(x) = Q_D(x)` for every reachable state x — i.e., the two
   sample paths are LITERALLY THE SAME random realization (same
   inflows, demands, capacities at every (phase, block)).

Condition (2) is **NOT met whenever any per-scenario input series
differs across scenes** — even for "structurally similar" runs like
juan/gtopt_iplp where 16 scenes share the same network/generators/
demand but draw from 16 different historical hydrologies. Each
scene's Q_s is a different function because the conditioning sample
path differs, so cross-scene broadcasting is mathematically invalid.

This is **not a special-case "heterogeneous data" issue** — it is
the standard multi-cut SDDP formulation. The literature does not
permit cross-scenario cut sharing in multi-cut SDDP. The
`accumulate`/`expected`/`max` modes try to mimic single-cut SDDP
(one shared α bounding `E[Q]`) inside a multi-cut architecture,
which is the architectural mismatch.

---

## Phase 0 — Lock in the shipped workaround (status: SHIPPED 2026-04-30)

Already done:
1. `support/juan/gtopt_iplp/gtopt_iplp.json` :
   `cut_sharing_mode: max` → `none`.
2. `include/gtopt/sddp_types.hpp` SDDPOptions doc-block warning.
3. `feedback_cut_sharing_unsafe.md` agent-memory entry.
4. `test/source/test_sddp_bounds_sanity.cpp` WARN-only regression
   guard.
5. `.claude/agents/lp-numerics-expert.md` expanded with
   "SDDP-specific defects: bound asymmetry and cut-sharing
   validity" section + two-line verdict (`SDDP BOUND VERDICT`).

Verification (do this once):
- `ctest --output-on-failure -j20 -R "bounds sanity"` — all 3
  tests pass; the WARN-only known-issue subcases emit the
  expected `LB > UB` warnings without failing CI.
- Re-run juan/gtopt_iplp end-to-end; gap converges monotonically;
  no `SIGNIFICANT negative gap` log lines.

---

## Phase 1 — Make the bug LOUD (status: SHIPPED 2026-04-30)

Goal: a user who opts in to a non-`none` mode in JSON cannot do so
silently.

### 1.1 Runtime warning at SDDP setup — DONE

`source/sddp_method.cpp::initialize_solver` emits a `SPDLOG_WARN`
when `cut_sharing != none && num_scenes > 1`. The warning names
the mode, states the precondition, and links to this plan and the
regression test.

### 1.2 Comment correction in `sddp_cut_sharing.cpp` — DONE

The misleading justifications at lines 87-118 (`accumulate`),
141-152 (`expected`), and 196-216 (`max`) have been replaced with
**VALIDITY WARNING** blocks explaining the multi-cut-vs-single-cut
architectural mismatch and citing this plan.

### 1.3 Strict configuration-error gate — DROPPED

A `strict_cut_sharing_check` flag was considered but not shipped.
The runtime WARN is sufficient: it appears once per `solve()` call
and is impossible to miss in any structured log scrape. Hard
errors would break existing fixtures
(`test_sddp_per_cell_release.cpp`, `test_sddp_bounds_sanity.cpp`'s
identical-Q subcases) without delivering proportionate value.
Reconsider if a juan-scale silent miscompile slips past code
review again.

### 1.4 Docs — DONE

- `docs/methods/sddp.md`: added a "Cut-sharing validity warning"
  callout under the `cut_sharing_mode` table; existing per-mode
  rows annotated with **KNOWN INVALID** for distinct-sample-path
  runs.
- `include/gtopt/sddp_enums.hpp`: extended the `CutSharingMode`
  doxygen block with the multi-cut-architecture warning.
- Inline cross-references to this plan from
  `sddp_cut_sharing.cpp` comments and `sddp_types.hpp`
  `cut_sharing` field doc.

### 1.5 Tests — DONE

Three new doctests added to
`test/source/test_sddp_bounds_sanity.cpp`:

1. `SDDP cut_sharing WARN — fires for multi-scene non-none modes`
   — uses `LogCapture` to assert the warning text appears for
   each of `accumulate` / `expected` / `max` on the 2-scene 3-phase
   fixture.
2. `SDDP cut_sharing WARN — silent for cut_sharing=none` — the
   warning must NOT fire for `none` mode.
3. `SDDP cut_sharing WARN — silent for single-scene runs` — the
   warning must NOT fire when `num_scenes == 1` even with
   `cut_sharing=max` (sharing is a no-op).

All three pass. Total SDDP test count is now 240 (was 237 before
Phase 1).

### 1.6 Acceptance — VERIFIED

- juan/gtopt_iplp run with `cut_sharing=none` (shipped) is
  unchanged.
- A user JSON with `cut_sharing=max` and >1 scene emits the
  WARN line.
- All existing tests pass; new doctests pass.

---

## Phase 2 — Probability rescale (DROPPED — see below)

Goal: extend cut-sharing validity from "homogeneous prob +
homogeneous dynamics" to "homogeneous dynamics only" via a
per-destination prob rescale on the broadcast cut.

**Status: DROPPED 2026-04-30.**

Phase 2 would have plumbed a `source_prob` field through
`SparseRow` so `share_cuts_for_phase` could rescale broadcast
cuts by `prob_D / prob_S` per destination. Detailed design is
preserved in git history; summary:

- The fix would extend cut-sharing validity from
  "identical sample paths" to "same dynamics, different
  probabilities only".
- It does NOT help juan or any production multi-scenario run
  whose scenes draw distinct sample paths (the typical case).
- It only matters for synthetic test fixtures with deliberately
  duplicated dynamics but unequal probabilities — a
  configuration nobody actually runs.
- It would add permanent plumbing (`source_prob` on every
  cut-emitting site, an O(N_scenes²) per-destination rescale
  loop in `share_cuts_for_phase`) for narrow utility.

Skip rationale: maintaining production code for a degenerate
test-only configuration is not justified. The Phase 1 WARN
already prevents accidental opt-in.

---

## Phase 3 — Shared-α with per-scene cut registration (~1+ week)

Goal: enable mathematically valid cross-scene cut sharing for
production multi-scenario runs (juan-style) by introducing a
**shared α column** at the planning level, keeping per-scene LPs
for parallelism.

### Architectural change

Today:
- `α^k_s` is a per-scene column inside scene s's phase-k LP.
- Cuts on α^k_s are scene-local. Each cut bounds `Q_s(x)` —
  scene s's value function along scene s's specific sample path.

Phase 3:
- One shared `α^k` column owned by `PlanningLP`. Every scene's
  phase-k LP references the same column.
- Cuts from scene s's backward pass land on the shared α^k.
- The cut RHS uses `obj_phys_s = prob_s · Q_s*(x_s_trial)` — each
  scene's cut is its own valid lower bound on the shared α^k
  (which bounds the probability-weighted sum, not E[Q]
  per-scene).
- Master phase-0 LP per scene returns `phase_0_obj_phys_s`
  including the shared α^0 contribution. LB aggregation in
  `compute_iteration_bounds` is unchanged at the read site
  (still sums `phase_0.get_obj_value()` across scenes), but
  semantically the LB is now `Σ_s prob_s · opex^0_s + α^0`
  with α^0 a single shared bound.

### Why this is valid

This IS the canonical Pereira-Pinto multi-cut formulation: cuts
from each scenario contribute to a shared cost-to-go function;
the master picks the max-active cut. The current per-scene-α
implementation is a *deviation* from the canonical form that
provides parallelism but forfeits cut sharing.

### Files that change

- `include/gtopt/sddp_alpha.hpp` (or wherever α is defined) —
  add a `SharedAlpha` mode alongside the per-scene mode.
- `source/sddp_method_alpha.cpp` — install the shared α column
  on `PlanningLP` rather than per scene; reference it from
  every (scene, phase) LP at build time.
- `source/sddp_method_iteration.cpp` — backward-pass cuts now
  emit on the shared α; remove the per-scene α_col lookup in
  `build_benders_cut_physical`.
- `source/sddp_cut_sharing.cpp` — under shared-α mode, cuts
  from any scene's backward pass directly land on the shared α
  with NO broadcast loop. The `accumulate`/`expected`/`max`
  modes become irrelevant — there is exactly one valid mode
  (which is "all cuts on the shared α").
- `source/sddp_method_iteration.cpp::compute_iteration_bounds`
  — verify the LB readback handles the shared α correctly.
- `include/gtopt/sddp_options.hpp` — add
  `bool shared_alpha {false}` (default off; opt-in for now).

### Tests

- A new TEST_CASE that verifies LB ≤ UB with `shared_alpha=true`
  on the synthetic 0.6/0.4 fixture and on a heterogeneous-
  inflow 2-scene fixture.
- Verification that `shared_alpha=true` converges in
  comparable iterations to the current `none` mode on
  identical-Q fixtures (no regression).
- Performance benchmark: end-to-end wall clock on
  juan-IPLP-small with `shared_alpha=true` vs `none`.

### Compatibility

- Default off. Existing JSON configs are unchanged.
- Documented as "experimental, may converge faster than
  per-scene α at the cost of less parallel master solves."
- Keep per-scene-α as the default for at least one release
  cycle while collecting performance data.

### Effort

4–6 days for the LP-assembly change, 2 days for the
`SDDPMethod` plumbing and tests, 2 days for benchmarks.

### Why this is the *recommended* sharing path

Because:
- It IS the canonical multi-cut SDDP formulation. The current
  per-scene-α implementation is the deviation.
- The math is unambiguous; no rescaling, no fingerprinting, no
  approximation. Cuts from any scene are valid lower bounds on
  the shared α by Pereira-Pinto's original proof.
- Performance is competitive: per-scene parallelism is
  preserved on the forward pass; the only serialized step is
  the master α — which is exactly the same situation as in
  every other production SDDP implementation (SDDP.jl,
  StochOptFormat, PSR's SDDP).

### 3.1 Decision: which P2 architecture?

The audit listed three options. Pick one before implementation:

**Option A** — monolithic α + per-scene scenarios
(Pereira-Pinto multi-cut). Single shared α_phase column;
expectation cut summed across scenes is mathematically valid
because all scenes contribute to the SAME α. Requires:
- Replacing `α^k_s` per-scene-LP columns with one `α^k`
  column referenced from every scene's LP.
- LP-assembly changes in `sddp_method_alpha.cpp` and the
  state-variable registry.
- Bound-aggregation changes in `compute_iteration_bounds`.
- Cut-store changes (cuts now belong to (phase, NOT scene)).

**Option B** — dynamics-fingerprint scene grouping. Scenes are
partitioned at setup time into groups with identical dynamics
fingerprints; cut sharing is restricted within a group. For
juan, all 16 scenarios are different ⇒ each is its own
group ⇒ sharing is a no-op (equivalent to `none`).

**Option C** — Phase 2's prob rescale + dynamics-fingerprint
guard. Only the (prob, dynamics) homogeneous case is
broadcast; distinct-Q cuts stay scene-local. This is the
minimum correct extension of P1.

Recommended: **Option C** (lowest risk, retains Phase 2's wins,
makes the system safely opt-in for any homogeneous subset).
Option A is the gold standard but is a structural rewrite.

### 3.2 Dynamics fingerprint (Option C)

Define a per-scene fingerprint:
```cpp
size_t scene_dynamics_hash(SceneLP, PlanningLP) const;
```
Hash inputs:
- All `Flow::discharge` / `inflow` schedules touching the scene's
  scenarios.
- All `Demand::capacity` schedules touching the scene's
  scenarios.
- All `Generator::capacity` and `gcost` schedules touching the
  scene's scenarios.
- All `Reservoir::eini`/`emin`/`emax`/`efin` per scene.
- (Discount/duration are phase-level, not scene-level — exclude.)

Two scenes have "homogeneous dynamics" iff their fingerprints
match.

Cache the fingerprint on `SceneLP` at build time so the cut
sharing path does an O(1) compare.

### 3.3 Cut-sharing gate

In `share_cuts_for_phase`, group destination scenes by
fingerprint. Only broadcast a cut from S to D when
`fingerprint(S) == fingerprint(D)`.

Equivalently, partition `scene_cuts` by fingerprint group; run
the existing accumulate / expected / max logic per-group; then
no inter-group broadcast.

### 3.4 Tests

- Re-enable the distinct-Q-dynamics test from Phase 2.4 — it
  should now pass (because no inter-group sharing occurs, the
  cuts behave like `none` mode for distinct-Q scenes).
- Add a TEST_CASE with a 4-scene fixture: scenes 1+2 share
  fingerprint A, scenes 3+4 share fingerprint B. Verify cut
  sharing reduces gap within each group while distinct-Q
  pairs do not interact.

### 3.5 Acceptance

- All `cut_sharing` modes produce strict `LB ≤ UB` on every
  fixture in `test_sddp_bounds_sanity.cpp`.
- juan/gtopt_iplp can be safely flipped to `cut_sharing=max`
  with no LB overshoot (all 16 scenes are own-group, sharing
  is a no-op equivalent to `none`).
- A genuine homogeneous-subset case (e.g., 4 scenes representing
  noise replicates of the same hydrology) gets faster
  convergence from sharing.

### 3.6 Risks

- Fingerprint design must capture every input that affects the
  per-scene cut. Missing a field ⇒ false-positive sharing ⇒
  reintroduce LB overshoot. Mitigation: make the fingerprint
  function a single point of truth and unit-test that it
  changes when any cost/inflow/demand/capacity field
  changes.
- Hash collisions are mathematically possible; use a strong
  hash (boost::hash_combine on raw bytes is fine for setup-time
  use).

---

## Test conversions roadmap

| File | Pre-Phase-1 | Current (Phase 1 SHIPPED) | After Phase 3 (deferred) |
|------|-----------|---------------|---------------|
| `test_sddp_bounds_sanity.cpp` 2s3p 0.6/0.4 | WARN | WARN | CHECK |
| `test_sddp_bounds_sanity.cpp` 2s10p identical-sample-path | CHECK | CHECK | CHECK |
| `test_sddp_bounds_sanity.cpp` cut_sharing WARN emission | (new) | CHECK | CHECK |

Phase 2's planned WARN→CHECK conversion is moot because Phase 2
was dropped.

---

## Effort estimate

| Phase | Status | Engineering | Risk |
|-------|--------|-------------|------|
| Phase 0 | SHIPPED | done | — |
| Phase 1 | SHIPPED | ~0.5 day | LOW (pure docs/log) |
| Phase 2 | DROPPED | n/a | n/a |
| Phase 3 | DEFERRED | 4–6 days if/when justified | HIGH |

Phase 3 (shared-α architectural change) is genuinely optional.
Re-evaluate only if a juan-style run with `cut_sharing=none`
proves too slow to converge in production wall-clock budgets.

---

## Definition of Done

For each phase, DoD requires:
1. All new code is doctest-covered.
2. All new tests use `ctest -j20` (per `feedback_no_serial_tests`).
3. The full SDDP test suite (`ctest -R "[Ss]ddp|SDDP"`) passes.
4. Manual juan/gtopt_iplp end-to-end run shows monotone gap
   convergence with no `SIGNIFICANT negative gap` warnings.
5. Memory updated:
   - `feedback_cut_sharing_unsafe.md` revised when each phase
     changes the user-facing rule.
   - `MEMORY.md` index entry stays current.
6. Relevant docs updated in lockstep:
   - `docs/methods/sddp.md`
   - `include/gtopt/sddp_enums.hpp` doxygen
   - `include/gtopt/sddp_types.hpp` `SDDPOptions::cut_sharing`
     doc-block

---

## References

- `lp-numerics-expert` deep audit (in-conversation, 2026-04-30,
  not committed since `support/` was hook-blocked at
  agent-write time).
- `test/source/test_sddp_bounds_sanity.cpp` — synthetic
  regression test.
- `feedback_cut_sharing_unsafe.md` — agent feedback memory.
- `.claude/agents/lp-numerics-expert.md` — expanded
  "SDDP-specific defects" section with diagnostic playbook.
- Pereira & Pinto (1991) — original SDDP, multi-cut variant
  uses one shared α per stage.
- Guigues, de Matos, Philpott (2019) "Single cut and multicut
  SDDP" — canonical reference for cut-sharing semantics.
