# SDDP cut-validity findings ledger — 2026-07-08

Continuation of the certification campaign handed off in
[`sddp_cut_validity_handoff_2026-07-07.md`](sddp_cut_validity_handoff_2026-07-07.md).
This ledger records every ε-tolerance, every place a uniform-probability
assumption enters, and every code-comment claim the theorem document
(`docs/formulation/sddp-cut-validity.md`) must prove or refute — with
file:line citations as of commit `e93290fe` (branch
`handoff/sddp-cut-validity`).

Reading status: the handoff's "Not yet read" list is COMPLETE, with two
exceptions noted in §6.

---

## 1. Verified results (previously suspected / derived)

### 1.1 Forward-pass UB strip subtracts only `varphi_0` — CONFIRMED DEFECT

- `source/sddp_forward_pass.cpp:884-895`: the opex strip computes
  `alpha_val = find_alpha_state_var(sim, scene, phase)->col_sol() × sa`.
- The 3-arg overload (`source/sddp_method.cpp:57-74`) resolves the key
  `uid = sddp_alpha_uid` exactly — i.e. offset 0, `varphi_0` only, and
  the strip applies it at **full weight** (`× scale_alpha`, no 1/N).
- Under `cut_sharing = multicut` the LP objective's future term is
  `(1/N)·Σ_s varphi_s` (`source/sddp_method_alpha.cpp:174-175`:
  `unit_cost = 1.0 / n_alpha`).  The correct strip is therefore
  `(1/N)·Σ_s varphi_s·scale_alpha` — obtainable via `alpha_cols_on_cell`
  (`source/sddp_method.cpp:134-152`).
- The two coincide only while all `varphi_s` are equal (iteration 0,
  all bootstrap-pinned `[0,0]`; also identical scenes × uniform
  probabilities, where symmetric cut sets keep every `varphi_s` at the
  same floor).  Once the per-scenario values diverge — non-uniform
  probabilities do this even on identical dynamics, since
  `varphi_r ≈ p_r·Ṽ` — `forward_objective` (and hence the UB and the
  gap) is biased by `(w·Σ_s varphi_s − varphi_0)·scale_alpha` per
  (scene, phase), either sign (`w` = the M4 weight, §1.2).  Single-α
  modes are unaffected (`n_alpha = 1`, cost 1.0).
- Status: **FIXED 2026-07-08** (post-oracle, as mandated): the strip
  now walks the cell's α registry (contiguous uids from
  `sddp_alpha_uid`, same convention as `alpha_cols_on_cell`) and
  removes `w·Σ_s varphi_s·scale_alpha`, with `w` read from the SAME
  shared `alpha_unit_cost` helper that priced the columns
  (`register_alpha_variables`) — pricing and strip cannot diverge.
  Regression gate: identical-dynamics UB-parity test in
  `test_sddp_bounds_sanity.cpp` (multicut UB ≡ none UB per iteration
  from iteration 2 on; 0.6/0.4 fails pre-fix, passes post-fix).

### 1.2 Multicut 1/N derivation — CONFIRMED, fix statement refined

All code inputs to the handoff's centerpiece derivation verified:

- Each scene-LP carries N columns `varphi_s`, each priced `1/N`
  regardless of scene probabilities
  (`source/sddp_method_alpha.cpp:148-231`).
- Scene-S's backward cut lands on S's own `varphi_S` at the previous
  phase (`source/sddp_method_iteration.cpp:593-631`), with
  `z* = forward_full_obj_physical` = the **full** target-LP objective
  including its own `(1/N)·Σ_r varphi_r` future term — so the recursion
  in the derivation is exactly what the code implements.
- `share_cuts_for_phase` broadcasts every cut to every scene-LP, same
  `varphi_S` column (`source/sddp_cut_sharing.cpp:235-342`).
- Scene-LP costs are `p_s`-folded via
  `cost_factor = probability × discount × duration`
  (`include/gtopt/cost_helper.hpp`), so `varphi_r` is bounded by cuts on
  the `p_r`-folded value `V_r`.

Refined fix statement (the handoff's wording was ambiguous): writing
`V_s(t)` for scene-s's `p_s`-folded phase value and `Ṽ_s = V_s/p_s`,
pricing `varphi_r` at weight `w_r` in scene-s's LP gives

    Ṽ_s(t) = min [ c_s + Σ_r (w_r·p_r/p_s)·Ṽ_r(t+1) ].

For this to be the Bellman recursion of the stagewise-independent
process resampled with measure `q_r = p_r` we need `w_r·p_r/p_s = p_r`,
i.e. **`w_r = p_s` for every r — the probability of the scene that OWNS
the LP, uniform across the N varphi columns** (not `p_r` per column).
Under uniform probabilities `p_s = 1/N` this collapses to the legacy
1/N pricing.  The LB `Σ_s V_s(0) = Σ_s p_s·Ṽ_s(0)` is then the
resampled-process expected value under a uniform initial-scene draw —
a valid LB **for that process**.

Status: **IMPLEMENTED 2026-07-08** — the pricing rule lives in the
shared free function `alpha_unit_cost` (`sddp_types.hpp` /
`source/sddp_method_alpha.cpp`), consumed by
`register_alpha_variables` for both the intermediate
(`CutSharingMode::multicut`) and terminal
(`BoundaryCutSharingMode::multicut`) α columns; probabilities are
normalized over scenes with a `Σp > 0` guard (fallback 1/N).  Gated by
the failing-then-passing identical-dynamics 0.6/0.4 oracle case in
`test_sddp_cut_oracle.cpp` (fails under 1/N pricing, strict-passes
under M4).  The former `initialize_solver` non-uniform WARN became an
INFO (theorem doc §8, Prop. M4).

Consequences confirmed (the second bullet is HISTORICAL — it described
the pre-M4 `1/N` pricing and is retained as the record of why M4
shipped):

- Uniform `p_s = 1/N`: multicut LB is valid for the resampled process;
  LB > UB against the persistent-sample-path UB is a process mismatch,
  not a cut bug.  This keeps the heterogeneous-scene multicut test
  WARN-only *permanently* (the comparison is between two different
  stochastic processes).
- (pre-M4) Non-uniform `p_s` under `1/N` pricing: the future term was
  inflated by `1/(N·p_s) > 1` for scenes with `p_s < 1/N` → LB not
  valid for any process.  The `test_sddp_bounds_sanity.cpp` multicut
  WARN case uses 0.6/0.4 — exactly that configuration; since the M4
  fix it is certified for the `q_r = p_r` resampled process and only
  the Corollary-M2 process mismatch keeps it WARN-tier.

---

## 2. ε-validity registry

Every site where a cut is deliberately made valid only up to a
quantifiable tolerance.  The theorem document states each cut's
underestimator property modulo the applicable entries; the oracle
harness (deliverable 2) must assert with tolerances derived from these.

| # | Site | Mechanism | Bound on over-tightening |
|---|------|-----------|--------------------------|
| E1 | `build_benders_cut_physical` ×3 (`source/benders_cut.cpp:161,207,276`) | drop link when `abs(rc_phys) < cut_coeff_eps` (ABSOLUTE); both the coefficient and its `rc·v̂` RHS correction are skipped | `Σ_dropped eps·abs(x_i − v̂_i)` ≤ `eps·Σ_dropped diam(box_i)` |
| E2 | `add_cut_row` install filter (`sddp_method_iteration.cpp:669-674` passes `ceps`; `SparseRow::to_flat(eps)` in `linear_interface.cpp:1980`) | drop physical coefficient `abs(c) < eps` at install; RHS unchanged | `eps·abs(x_i)` per dropped column (NOT recentered at v̂) |
| E3 | Parquet loader re-filter (`source/sddp_cut_parquet.cpp:904-918`) | drop when `abs(c) < cut_coeff_eps × row_max` (RELATIVE, incl. the α coefficient in `row_max`); RHS unchanged | `eps·row_max·abs(x_i)` per dropped column |
| E4 | Loader unresolved-coefficient drop (`sddp_cut_parquet.cpp:893-901`) | coefficient whose `(cls,var,uid)` has no state variable in the destination is silently dropped, RHS keeps full z* (WARN log only) | unbounded a priori — `abs(g_i)·abs(x_i)`; must be characterized as a *precondition* (state-variable universe preserved across save/load), not a tolerance |
| E5 | Per-link feasibility-cut outward perturbation (`benders_cut.cpp:1225`: `kFactEps = 0.01·cut_coeff_eps·niter`) | RHS pushed OUTWARD, escalating with repeat emissions, to escape cycling | deliberate over-tightening by `kFactEps·abs(pi)`-order; grows without bound with `niter` — theorem must state per-link cuts are heuristic separators, not exact |
| E6 | Boundary-cut CSV install filter (`sddp_boundary_cuts.cpp:652-657`, audit P2-1) | same `cut_coeff_eps` absolute filter as E2 at install | as E2 |
| E7 | Aperture per-aperture cuts (E1 applied per aperture BEFORE weighted averaging, `sddp_aperture.cpp:789-793`) | averaging inherits the per-cut error | ≤ `max_a` (E1 bound), weights convex |
| E8 | Dual-shared synthesized intercept (`dual_shared_bound_correction` + synthesis site, `sddp_aperture.cpp`; Lemma AP2) | intercept = `z*_rep + Σ d⁺·Δl + Σ d⁻·Δu` at the representative's shared dual point | 0 on exact vertex duals (weak duality is exact there); on ε-optimal interior duals (PDLP/cuOpt, F10) using the primal `z*` over-states by ≤ the primal–dual gap; inherits E1 on `z*`, `d`.  Precondition: column-bound deltas only — row-RHS deltas are gated out (F12) |
| E9 | Strengthened-cut intercept (`build_strengthened_benders_cut`, `benders_cut.cpp`; caller pins `mip_gap = 1e-9`, `mip_gap_abs = 1e-6`, `sddp_method_iteration.cpp`) | intercept = `max(b_LP, m*)` with `m*` the MIP **incumbent** Lagrangian value (not the best bound) | over-tightens by ≤ `max(mip_gap·|m*|, mip_gap_abs)`; MIP failure/timeout falls back to the SB1-valid LP-relaxation cut (0 extra ε) |

Interaction note: E1 (emit) and E3 (reload) COMPOUND across a cascade
level transition — a coefficient surviving E1 can be dropped by E3
relative to the row max; the tolerances add.

## 3. Uniform-probability / measure assumptions registry

| # | Site | Assumption |
|---|------|------------|
| U1 | `sddp_method_alpha.cpp:148-153,174-175` | `varphi` priced `1/N` "so Σ (1/N)varphi reconstructs the expected cost-to-go" — true only for uniform `p_s` (see §1.2); the comment silently assumes it |
| U2 | `sddp_cut_sharing.cpp:258-260` | "(1/N)Σ_s Q_s = E[Q]" — same silent uniformity; the theorem doc corrects this to the resampled-process statement |
| U3 | Aperture weights (`sddp_aperture.cpp:562,972-977`) | weight = `count × probability_factor`, renormalized over the FEASIBLE-and-selected subset only — the expected cut is w.r.t. a conditional measure (see F3, F4) |
| U4 | `num_apertures` first-N selector (`sddp_aperture.cpp:95-104`) | truncates each phase's aperture list (wettest-first after plp2gtopt sort) then renormalizes — a deliberate measure change; LB statements are relative to the truncated process |
| U5 | LB/UB aggregation (`sddp_method_iteration.cpp:1786-1846`) | plain SUM of `p_s`-folded per-scene values; implicitly assumes `Σ_s p_s = 1` for the sum to be an expectation (diagnostic `scene_probability_lost` normalizes by `total_p`, the bounds do not) |
| U6 | Infeasible scenes contribute 0 to both bounds (`sddp_method_iteration.cpp:1803-1808,1828-1831`) | bounds are `E[cost·1_feasible]` — mass on infeasible scenes surfaced separately, not in the bounds |

## 4. Code-comment claims: prove or refute in the theorem doc

| # | Claim (site) | Verdict |
|---|--------------|---------|
| C1 | `state_variable.hpp:310-325`: `reduced_cost_physical` is NOT truly physical — `cost_factor` folded in; validity "relies on cost_factor cancelling at the destination master LP" | PROVE.  The cut lands on the master whose own objective is identically `p_s`-folded; z* and rc carry the same folding, so the cut bounds the folded value function — no cancellation actually occurs, both sides are folded.  Needs the precise statement (same scene, same discount/duration chain between consecutive phases). |
| C2 | `sddp_method_alpha.cpp:148-153`: "that routing is what keeps the bound valid" (multicut) | PARTIALLY TRUE — routing gives per-scenario cut separation, but validity additionally requires uniform probabilities (§1.2).  REFUTE as stated; replace with resampled-process statement. |
| C3 | `sddp_cut_sharing.cpp:258-260`: "(1/N)Σ Q_s = E[Q] — a VALID lower bound … for heterogeneous scenes" | REFUTE as stated (uniformity + process mismatch); see U2. |
| C4 | `sddp_method_alpha.cpp:277-297`: terminal α ≥ 0 floor "always valid" because "every stage cost is non-negative" | PROVE conditionally.  Valid iff all stage costs ≥ 0.  Negative-cost inputs (negative `gcost`, revenue-like terms) or a rebased/user FCF violate the precondition — the code itself releases the USER α to go negative (`sddp_method_alpha.cpp:108-114`) and mean-shifted boundary cuts make negative α legitimate.  State as an explicit assumption. |
| C5 | `sddp_method_alpha.cpp:574-576`: `bound_above` ceiling "never cuts the optimum" | PROVE (short): at a min-optimum with α priced > 0, α* sits on its highest binding cut k*: α* = rhs_k* − Σ g_j x*_j ≤ rhs_k* − inf_box Σ = ceil_k* ≤ max_k ceil_k.  Requires: α appears only in ≥-cut rows and the objective; x* inside the bound box. |
| C6 | `apply_alpha_floor` floor accumulation (`sddp_method_alpha.cpp:567-593`) | PROVE.  Each cut k implies α ≥ floor_k over the whole state box; the code takes **min_k** (name `tightest_floor_phys` is misleading — it accumulates the LOOSEST), which is weaker than the also-valid max_k, hence safe. |
| C7 | Floor fallback 0.0 (`sddp_method_alpha.cpp:568` init + `:622-628` write) | CAVEAT.  When no referencing cut yields a bounded box-floor (`any_cut_floor == false`), the column floor is written as 0.0 — same non-negativity precondition as C4; over-tightens legitimately-negative (rebased) α in that corner.  Document as an assumption; candidate for a follow-up fix (write −∞ or the seed only when C4's precondition holds). |
| C8 | `sddp_boundary_cuts.cpp:831-836`: α priced "cost = 1.0" so the mean-shift objective drop is exactly −c̄ | IMPRECISE under terminal multicut: each terminal `varphi_s` is priced 1/N, but shifting every cut on the LP by the same c̄ drops each varphi's feasible min by c̄, so the future term `(1/N)·Σ_s varphi_s` still drops by exactly c̄ — net claim holds, argument needs the sum.  PROVE with the corrected argument. |
| C9 | `sddp_method_iteration.cpp:1833-1837`: c̄ folded into the master's native objective offset at boundary-cut load → `get_obj_value()` already restored, no double count | VERIFIED by reading both sites (fold at load, plain read at bounds).  State in the persistence section. |
| C10 | Per-link feasibility cuts (PLP `AgrElastici` mirror, `benders_cut.cpp:1170-1578`) | OPEN.  Handoff's substitutable-reservoir over-cut concern stands; guards found: BoxEdgeStats family-drop, saturation drop (`:1511-1516`, strict `>` + `box_eps`), escalating `kFactEps` (E5).  Theorem doc must present the counterexample analysis; E5 already makes exactness unattainable — state per-link cuts as heuristic separators with quantified slack. |

## 5. Other findings (new this session)

- **F1 — Parquet reload broadcast reconstruction** (`sddp_cut_parquet.cpp:
  669-695, 1013-1020, 1096-1119`): inherited multicut optimality cuts are
  re-broadcast at load — origin copy installed AND stored, non-origin
  copies installed only (never persisted).  Matches the in-run
  `share_cuts_for_phase` semantics; keeps saves origin-only (no N×
  blow-up).  Feasibility cuts are NOT broadcast (correct: they are
  scene-local).
- **F2 — `# scale_objective=` metadata is provenance-only**: stamped at
  save (`sddp_cut_parquet.cpp:119-126,361,401`), never read at load.
  Correct by design: `StoredCut` holds the pre-compose PHYSICAL row
  (`sddp_cut_store.hpp:97-117`), and `add_row`'s compose path re-applies
  whatever scale the reloading run uses (`linear_interface.cpp:
  1938-2125`).  Cross-`scale_objective` reload is therefore sound.
- **F3 — Aperture timeout treated as infeasible** (`sddp_aperture.cpp:
  892-916`): a timed-out aperture (finite true value) is dropped and the
  weights renormalized over the surviving subset — can over-tighten the
  expected cut when the dropped aperture is cheaper than the surviving
  average.  Genuine infeasibility is safe (V = +∞ dominates any cut).
  Theorem doc: state the conditional-measure caveat; harness: avoid
  timeouts.
- **F4 — Aperture duplicate memo & weights** (`sddp_aperture.cpp:
  459-470, 565-574`): duplicates reuse the solved cut with their own
  instance weight — sums to `count × pf` total, consistent.
- **F5 — MIP relaxation before aperture cut build** (`sddp_aperture.cpp:
  672-683`): clones are relaxed to LP before duals/rc are read.  Cut
  underestimates V_LP ≤ V_MIP — valid (weaker) for the MIP recursion.
- **F6 — PLP prices varphi identically but leaves it FREE**
  (`~/git/plp_storage/CEN65/src/defprbpd.f:812-818`):
  `obj = (ScalePhi/ScaleObj)/NVarPhi`, bounds `±DINFTY`.  gtopt's
  bootstrap pin `[0,0]` → floor `[0,∞)`-style release is an extra
  (conditionally valid, see C4/C7) tightening relative to PLP.
- **F7 — Legacy parquet files without a `scene` column broadcast every
  cut to every scene as origin** (`sddp_cut_parquet.cpp:1021-1028`) —
  installed AND stored on all scenes; under multicut this is both a
  file-size multiplier and (for heterogeneous scenes) semantically the
  broadcast the in-run path never persists.  Acceptable for legacy-only;
  note as a compatibility caveat.
- **F8 — Combinators** (`benders_cut.cpp:1582-1708`): weighted average
  normalizes internally (idempotent with the aperture caller's own
  normalization); result copies `cuts.front().scale` — fine today since
  all inputs are built with `scale = 1`, but a latent mismatch if mixed-
  scale rows are ever combined.
- **F9 — WARN inventory** (`test/source/test_sddp_bounds_sanity.cpp`):
  soft (WARN) = heterogeneous scenes × {accumulate, broadcast_mean, max}
  and × multicut (0.6/0.4).  Strict (CHECK) = none-mode on 3 fixture
  shapes, identical scenes × all 5 modes, scale_alpha probes 1/10/100
  (plain + apertures + variable_scales), WARN-emission tests.  After the
  theorem: legacy-mode WARNs stay (invalid, settled), multicut
  heterogeneous WARN stays (process mismatch; the non-uniform
  unsoundness was fixed by M4 on 2026-07-08 — see §1.2);
  upgrade path is oracle-based extensive-form comparisons, not
  UB-comparisons.
- **F10 — cuOpt duals are ε-optimal; no warm start (2026-07-08
  addendum)**: since `52d3e0e9` the cuOpt plugin maps `LPAlgo::barrier`
  → `CUOPT_METHOD_CONCURRENT`; the winning method may be PDLP, whose
  duals satisfy optimality only to the PDLP tolerance and are NOT
  vertex duals (no crossover).  Cuts built from cuOpt solves therefore
  carry a solver-dependent ε-validity term on top of §2 (weak duality
  holds for the dual-feasible part; PDLP's approximate feasibility adds
  a residual bounded by the solver tolerance).  The plugin also has NO
  warm-start path (`set_basis`/hints are documented no-ops; every
  resolve rebuilds cold), so `aperture_solve_mode = warm` silently
  degrades to cold under cuOpt — the K-aperture backward bill is K full
  cold solves there, the motivating case for dual-shared aperture cuts
  (Infanger–Morton evaluation over the bound deltas).

- **F11 — forward resampling shipped (2026-07-08 addendum)**:
  `forward_sampling_mode = resampled` (default `persistent`,
  byte-identical) re-draws a probability-weighted scene realization at
  every phase boundary (deterministic splitmix64 draw keyed on
  (iteration, scene, phase) — stable across forward-pass backtracking
  and thread scheduling) and applies it to the forward LP through the
  `update_aperture` machinery (column-bound pins; the AR equality-row
  RHS under `Flow.inflow_model`, replay-covered via the `pending_rhs`
  channel).  The UB then estimates the
  same `q_r = p_r` resampled process the M4 multicut LB certifies
  (Corollary-M2 mismatch dissolved by construction).  The drawn id is
  cached on `PhaseStateInfo::sampled_scene`; the backward target
  re-solve re-applies it so the cut is provably built from the SAME
  realization.  v1: one sampled path per scene-driver per iteration;
  simulation pass restores persistent per-scene data; pure-Benders
  backward under heterogeneous dynamics stays WARN-tier (theorem doc
  §8 remark) — the certified heterogeneous route is the aperture
  backward pass.

- **F12 — AR(1) × dual_shared/screened apertures dropped the `yᵀΔb`
  row term (found by cross-feature soundness review 2026-07-08, fixed
  same day)**: `dual_shared_bound_correction` prices column-bound
  deltas only, but an AR-mode `FlowLP::update_aperture` rewrites the
  AR equality-row RHS (`rhs = a_b − φ·lag_ref`) and the AR flow
  columns are FREE — the synthesized intercept silently omitted
  `yᵀΔb` over the AR-row RHS deltas, a state-independent constant of
  arbitrary sign (the aperture inflow delta and the lag-reference
  delta pull in opposite directions), so the synthesized cut could
  over-tighten and inflate the LB.  The row-touch gate exempted FlowLP
  wholesale on the stale "bounds exclusively" assumption
  (pre-fix `sddp_aperture.cpp` `apply_aperture_update`).  Fixed by
  (a) extending the gate: `FlowLP::has_ar_rows(scenario, stage)` marks
  AR-mode flows row-touching, so any delivered aperture value trips
  `non_bound_write` → sticky exact-solve fallback; and (b) a setup
  guard in `SDDPMethod::initialize_solver` that WARNs and downgrades
  `dual_shared`/`screened` to `warm` whenever any `Flow.inflow_model`
  is present.  Regression gate: warm-vs-dual_shared/screened
  stored-cut parity under AR (`test_sddp_ar_inflow.cpp`, F12 guard
  parity test).  The row-dual upgrade — snapshotting `y` + row RHS in
  `DsRepresentative` and adding `yᵀΔb` for RHS-only deltas, making
  dual-shared *correct* (and fast) under AR — is sanctioned future
  work, not implemented.

- **F13 — `sddp_hydro_3phase` e2e goldens vs primal degeneracy
  (2026-07-08, robustness policy)**: the integration fixture
  (`cases/sddp_hydro_3phase`) has a PRIMAL-DEGENERATE optimum — hydro
  (gcost 5) substitutes thermal (gcost 50) at a constant 45/MWh saving
  in every phase, so the marginal water value is flat and any
  allocation of the reservoir budget across phases/blocks is an
  alternate optimum with the same total 323 100.  The trained-run
  base goldens (`golden_iter3/iter50/iter50_hotstart.json`) pinned the
  per-phase obj_value tails (212 325 / 103 125), the intermediate
  `efin` (135), and per-block dispatch — all on the flat face; solver
  pivot-order drift (observed: 217 725 / 107 850, and 214 350 /
  105 150 with clp at 104185c1, total unchanged) failed the 5 trained
  `e2e_sddp_hydro_3phase_*_validate` cases whenever `GTOPT_SOLVER`
  named no per-solver variant.  Policy (matches the per-solver
  variants, which adopted it first — see
  `golden_iter50_hotstart_clp.json` for the original diagnosis): pin
  only solver-invariant quantities — total objective, LB/UB/gap,
  per-phase statuses, drained horizon `efin = 0` (leftover water
  always has positive substitution value), and the phase-1 balance
  dual (= thermal gcost, strictly interior) — and DROP the per-phase
  split / intermediate-efin / dispatch pins.  The alternative (an
  `efin_cost` tie-breaker in the fixture + full golden regeneration)
  was rejected: it perturbs the objective every golden pins and hides,
  rather than documents, the degeneracy.

- **F14 — per-scene expansion-CAPEX over-weighting (2026-07-08 found,
  2026-07-09 FIXED)**: `CapacityObjectBase::add_to_lp`
  (`source/capacity_object_lp.cpp`) priced the `capacost` carrying-cost
  objective column with `CostHelper::stage_ecost(stage, 1.0)` —
  probability weight **1.0** — because capacity columns are built once
  per scene (the `scenario.is_first()` guard) and, in the all-scenarios
  monolithic LP, the dispatch probabilities that share those columns
  sum to 1.  In the per-scene SDDP LPs, however, each LP carries ONLY
  its own scenario(s) with the GLOBALLY-normalized weight `p_s` (one
  scenario per scene → `p_s`; e.g. 0.5), so dispatch/OPEX folds by
  `p_s` via `block_ecost` while CAPEX stayed at 1.0 — the carrying
  charge was over-weighted by `1/p_s` in **every** scene-LP.

  *Why the renormalization hypothesis is refuted*: neither
  `PlanningLP::renormalize_scenario_probabilities`
  (`source/planning_lp.cpp:670`) nor `validate_planning` renormalizes
  *within* a scene on the one-scenario-per-scene SDDP path.  The former
  rescales the whole active subset (all scenes together) to sum 1.0 —
  here the two `p_s = 0.5` already sum to 1.0, so it is a silent no-op —
  and `validate_planning` explicitly **skips** single-scenario scenes
  (`validate_planning.cpp:1717-1740`: "their `probability_factor`
  represents the global weight used for cross-scene aggregation").  So
  each scene's scenario genuinely carries `p_s`, not a per-scene 1.0.

  *Derivation (test-4 fixture, 2 identical dry scenes, `p_s = 0.5`,
  build `K = 24.333 MW`, dispatch physical 36 760, carrying 292)*:
  * per scene-LP (defect) = `0.5 × 36 760 + 1.0 × 292 = 18 672`;
  * `compute_iteration_bounds` sums scenes (each per-scene obj already
    has `p_s` baked in) → LB = UB = `18 672 × 2 = 37 344`;
  * extensive twin (1 scenario, `p = 1`) `v_full = 36 760 + 292 =
    37 052`;
  * `37 344 − 37 052 = 292` = exactly **one** duplicated carrying
    charge (`= K × 12 h × 1 $/MW-h`).  Integer variant: `K = 50` →
    over-count `600`, observed LB `37 960 = 37 360 + 600`.

  *Fix*: fold the carrying-cost objective coefficient by the owning
  scene's total probability mass —
  `CostHelper::stage_ecost(stage, 1.0, scene_prob)` with
  `scene_prob = sc.system().scene().probability_factor()`
  (`SceneLP::probability_factor()` sums the scene's active scenarios).
  Only ONE objective site changed (the `capacost_col` `.cost`); the
  `capacost_row` coefficient `−stage_expcap · stage_hour_capcost` and
  the `capainst_row`/derating coefficients are **physical** ($ or MW)
  constraint structure and must stay probability-free (folding there
  would double-count).  The `capainst_col` has `.cost = 0.0` (no
  objective contribution).

  *Monolithic invariance*: in the monolithic solve `scene_array` is
  empty → `create_scene_array` synthesises ONE scene with
  `count_scenario = dynamic_extent` covering every scenario, so
  `SceneLP::probability_factor()` sums all scenarios = 1.0 → identical
  to the previous `stage_ecost(stage, 1.0)`.  Result: after the fix
  each identical scene folds carrying by `0.5`, the plain cross-scene
  sum reconstitutes the single 292 (resp. 600) charge, and LB = UB =
  `v_full` (resp. `≤ v_mip`) — the theorem-mandated Benders ≡ SDDP
  identity.  Tests 4-5 in `test_sddp_expansion_sharing.cpp` upgraded
  their WARNs to CHECKs and dropped the over-count pins.

## 6. Not available / deferred

- `support/plp/juan/sddp_lb_overshoot_iter01_2026-05-01.md` — the
  `support/` tree does not exist on this machine; empirical iter-0/1
  cut data unavailable.  Not blocking: the theorem document needs no
  empirical data.
- `plp-agrespd.f` read at signature/structure level only (per-link
  sensitivity-cut mirror already established in the handoff; gtopt-side
  guards verified — C10).

## 7. Stale docs to fix in lockstep (from handoff, confirmed)

- `docs/methods/sddp.md` §3.3 cut-sharing table lacks `multicut`;
  §4.5 says "average" where the aggregation is a probability-folded
  SUM (`compute_iteration_bounds`); §4.6 is three lines and predates
  multicut.
