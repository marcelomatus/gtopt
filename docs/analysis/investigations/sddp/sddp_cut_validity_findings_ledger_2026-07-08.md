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
  all bootstrap-pinned `[0,0]`).  Once per-scenario cuts diverge,
  `forward_objective` (and hence the UB and the gap) is biased by
  `(mean_s varphi_s − varphi_0)·scale_alpha` per (scene, phase) — either
  sign.  Single-α modes are unaffected (`n_alpha = 1`, cost 1.0).
- Status: **defect, do-not-fix until the oracle harness exists**
  (deliverable 2) so the fix lands with a test that would have caught
  it.  The fix is mechanical: iterate `alpha_cols_on_cell`, weight 1/N.

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
Under uniform probabilities `p_s = 1/N` this collapses to the current
1/N pricing.  The LB `Σ_s V_s(0) = Σ_s p_s·Ṽ_s(0)` is then the
resampled-process expected value under a uniform initial-scene draw —
a valid LB **for that process**.  Still do-not-implement until the
theorem document and an oracle test exist.

Consequences confirmed:

- Uniform `p_s = 1/N`: multicut LB is valid for the resampled process;
  LB > UB against the persistent-sample-path UB is a process mismatch,
  not a cut bug.  This keeps the heterogeneous-scene multicut test
  WARN-only *permanently* (the comparison is between two different
  stochastic processes).
- Non-uniform `p_s`: the future term is inflated by `1/(N·p_s) > 1`
  for scenes with `p_s < 1/N` → LB not valid for any process.  The
  `test_sddp_bounds_sanity.cpp` multicut WARN case uses 0.6/0.4 —
  exactly the provably-unsound configuration.

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
  heterogeneous WARN stays (process mismatch + non-uniform unsound);
  upgrade path is oracle-based extensive-form comparisons, not
  UB-comparisons.

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
