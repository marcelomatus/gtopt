# HANDOFF PROMPT — SDDP cut-validity certification campaign (2026-07-07)

Copy everything below this line into a fresh Claude Code session in the
gtopt repo.

---

You are continuing an in-flight campaign: **certify gtopt's SDDP/cascade
Benders-cut machinery mathematically** instead of debugging it empirically
(the LB-overshoot saga of 2026-04/06 consumed four investigations). Three
deliverables, in order:

1. **`docs/formulation/sddp-cut-validity.md`** — theorem document. State
   and prove the underestimator property for every cut path: fresh backward
   cuts, aperture expected cuts, multicut sharing, cross-level cascade
   inheritance, Parquet reload/recover, boundary/named cuts. Cover the
   interactions with `scale_objective`/kappa, `cost_factor` probability
   folding, the alpha-rebased FCF `obj_constant`, and `cut_coeff_eps`
   filtering. Characterize the uniform-probability assumption in multicut
   (derivation below — verify and write it up rigorously).
2. **Extensive-form oracle property harness** — a doctest fixture that
   solves small randomized multi-scene/multi-phase instances by brute-force
   extensive form and asserts every emitted cut underestimates the true
   recourse function, across all modes/paths. Then upgrade the WARN-only
   pins in `test/source/test_sddp_bounds_sanity.cpp` (16 WARNs) to CHECK
   where the theory says it is sound.
3. **Extract `compute_link_feasibility_cut`** (and siblings) from
   `build_multi_cuts` in `source/benders_cut.cpp` (~:1170-1578) under the
   oracle's cover. Behavior-preserving.

## Established facts (verified in code 2026-07-07 — spot-check line numbers)

**Cut construction** (`source/benders_cut.cpp`):
- Optimality cut, 3 overloads of `build_benders_cut_physical` (:137, :176,
  :247): row `alpha + Σ(−rc_i)·x_i ≥ z*_phys − Σ rc_i·v̂_i`, i.e.
  `alpha ≥ z* + Σ rc_i(x_i − v̂_i)`. rc = reduced cost of the *dependent*
  column (pinned lo=hi=v̂ by `propagate_trial_values`), which is the
  subgradient of the phase LP value wrt the pin — classical validity by LP
  convexity + duality, recursion closes by induction (installed cuts on the
  target's own alpha underestimate its future term).
- `cut_coeff_eps` drops links with |rc| < eps but keeps the full z* in the
  RHS → the emitted cut can over-tighten by up to eps·|x_i − v̂_i| per
  dropped link. **The correct statement is eps-validity, not exact
  validity.** The doc must state this precisely (and the oracle must use a
  tolerance derived from it).
- Feasibility cuts: aggregated form `build_feasibility_cut_physical`
  (:312-471, row duals pi on fixing rows `dep + sup − sdn = v̂`, per-link
  clamp to `[source_low, source_upp]`); per-link form `build_multi_cuts`
  (:1170-1578, PLP `plp-agrespd.f::AgrElastici` "sensitivity cut": one
  single-variable cut `pi·source ≥ pi·dep_clone_phys` per relaxed link).
  **Open validity question for the doc**: per-link splitting can over-cut
  when two reservoirs can substitute for each other (the elastic optimum
  picks ONE mix; per-link cuts assert every feasible master matches it).
  Analyze; find/construct a counterexample or a proof of the conditions
  under which it is exact. Note the guards already present: BoxEdgeStats
  family-drop, per-cut saturation drop (:1511), outward `kFactEps =
  0.01·eps·niter` perturbation (:1225) which *intentionally* over-tightens
  escalatingly to escape cycling — another eps-validity caveat.
- Elastic filter (`elastic_filter_solve` :908, `relax_fixed_state_variable`
  :475): Chinneck Phase-1 (objective zeroed), slacks priced
  `penalty × dep_scale_phys`, slack bounds from the physical source box,
  fixing row added RAW — dual-scaling convention documented at :705-718
  (`get_row_dual` returns ∂obj_phys/∂trial_LP; consumers divide by
  col_scale).
- Combinators (:1582-1708): `average_benders_cut` (1/n),
  `weighted_average_benders_cut` (normalized weights),
  `accumulate_benders_cuts` (plain sum). Convex combinations of valid cuts
  on the SAME alpha are valid; sums are NOT (that is the broken
  `accumulate` mode).

**Probability folding & bounds** :
- Every LP cost coefficient carries `cost_factor = probability × discount ×
  duration` (`include/gtopt/cost_helper.hpp:64-144`, `block_ecost`). A
  scene-LP objective ≈ p_s × physical cost.
- LB/UB aggregation is a plain SUM over feasible scenes — probability
  already baked in (`source/sddp_method_iteration.cpp:1781-1862`,
  post-2026-05-02 fix). **`docs/methods/sddp.md` §4.5 still says "average"
  — stale, fix it.** Also §3.3's cut-sharing table lacks `multicut`, §4.6
  is three lines and predates multicut entirely.
- Per-scene LB = master phase-0 `get_obj_value()`; the FCF alpha-rebase
  constant c̄ is folded into the master's native objective offset at
  boundary-cut load, so no double count (:1833-1837).

**Multicut architecture**:
- `register_alpha_variables` (`source/sddp_method_alpha.cpp:136-265`):
  under `cut_sharing=multicut` each scene-LP carries N columns
  `varphi_s` (uid = `sddp_alpha_uid + s`), each priced **1/N**
  (`unit_cost = 1.0/n_alpha`), bootstrap-pinned [0,0], `pin_scale=true`.
- Scene-S backward cut lands on S's OWN `varphi_S` at the previous phase
  (`source/sddp_method_iteration.cpp:593-631`); z* = cached
  `forward_full_obj_physical` (or re-solved under
  `backward_resolve_target`).
- `share_cuts_for_phase` (`source/sddp_cut_sharing.cpp:235-342`) broadcasts
  every cut to every scene-LP (multicut and max share the mechanics; only
  the alpha-column routing differs). Broadcast rows are never persisted;
  cross-level reload broadcasts loaded cuts (fix `9300306da`,
  `source/sddp_cut_parquet.cpp:686+`, `multicut_broadcast`).
- `apply_alpha_floor` (`sddp_method_alpha.cpp:421-640`): per-varphi
  cut-derived constant floor `min_k (rhs_k − Σ_j max(g_j·x_j))` over the
  state box — valid (each cut individually implies its floor). The
  `bound_above` ceiling claim ("alpha ≤ max_k ceil_k never cuts the
  optimum") needs a short proof in the doc.
- Terminal alpha ≥ 0 floor justified by non-negative stage costs
  (`sddp_method_alpha.cpp:277-297`) — check the tension with negative-value
  FCF/user-alpha cases (terminal USER alpha is deliberately released and
  may go negative; boundary cuts can carry negative rhs).

## Centerpiece derivation (verify independently, then write up)

Let V_s(t)(x) = optimal value of scene-s's phase-(t+1) LP at incoming
state x. Under multicut:
V_s(t)(x) = min [ p_s·c_s(u) + (1/N)·Σ_r varphi_r ] with cuts making
varphi_r ≈ V_r(t+1)(x'). So V_s(t)(x) = min [ p_s·c_s + (1/N)·Σ_r
V_r(t+1)(x') ].

- **Uniform p_s = 1/N**: substitute Ṽ_s := V_s / p_s = N·V_s. Then
  Ṽ_s(t)(x) = min [ c_s + mean_r Ṽ_r(t+1)(x') ] — exactly the
  nonanticipative Bellman recursion of the **stagewise-independent
  resampled process** (scene data redrawn uniformly at each phase
  boundary). Multicut LB is a valid LB *for that process*. The UB
  simulates persistent per-scene paths (no resampling) → LB > UB under
  heterogeneous scenes is a **process mismatch** (resampled vs persistent),
  not a cut-validity failure. This crisply formalizes the "expected, not a
  bug" folklore in the project memory.
- **Non-uniform p_s**: Ṽ_s(t)(x) = min [ c_s + (1/(N·p_s))·Σ_r p_r·
  Ṽ_r(t+1)(x') ]. The factor 1/(N·p_s) ≠ 1 inflates the future term for
  scenes with p_s < 1/N → the LB is NOT a valid lower bound (can
  overshoot). **Fix**: price each varphi_r in scene-s's objective at p_s
  (the destination scene's normalized probability) instead of 1/N — under
  uniform probabilities they coincide, and the recursion becomes exact.
  One-line change in `register_alpha_variables` (`unit_cost`), but do NOT
  implement until the doc proof + an oracle test exist.

## Suspected bug (verify before asserting)

`source/sddp_forward_pass.cpp:884-895`: the UB strip computes
`alpha_val = find_alpha_state_var(sim, scene, phase)->col_sol() ×
scale_alpha` — the offset-0 lookup, i.e. **only `varphi_0`, at full
weight**. Under multicut the objective's future term is (1/N)·Σ_s varphi_s.
The two coincide only when all varphi_s are equal (iter 0); they diverge
once per-scenario cuts differ — biasing `forward_objective` and hence the
UB. Verify `find_alpha_state_var`'s default-overload semantics, then either
confirm as a defect (strip should be Σ_s (1/N)·varphi_s·scale_alpha, i.e.
the objective's actual alpha contribution) or explain why it is right.

## Not yet read (do this before writing the doc)

- `source/sddp_cut_parquet.cpp` :660-1120 — load/save semantics, the
  `# scale_objective=` header, name-based coefficient round-trip, per-scene
  routing for `none` vs broadcast for multicut.
- `source/future_cost_lp.cpp` (225 lines) — FCF alpha-rebase / obj_constant.
- Aperture cut path in `source/sddp_method_iteration.cpp` (search
  `weighted_average_benders_cut`) — per-aperture cuts + probability
  weights; `num_apertures` first-N selector changes the measure
  (renormalization question for the doc).
- `include/gtopt/state_variable.hpp` — `reduced_cost_physical(scale_obj)` /
  `col_sol_physical` unit conventions.
- Boundary/named cut loaders (`load_boundary_cuts_csv`, named cuts) — alpha
  release + rebase mechanics.
- `test/source/test_sddp_bounds_sanity.cpp` (535 lines, 16 WARNs) —
  inventory what is pinned WARN-only and why.
- `LinearInterface::add_row` physical composition + `pivot_col` alpha-pivot
  equilibration (`source/linear_interface.cpp`) — needed for the
  scale-invariance section.
- Prior art: `docs/analysis/investigations/sddp/
  sddp_cut_sharing_fix_plan_2026-04-30.md` (architecture refresher +
  Phase-3 shared-alpha design that was superseded by multicut);
  `support/plp/juan/sddp_lb_overshoot_iter01_2026-05-01.md` (empirical
  iter-0/1 cut data). PLP reference: ~/git/plp_storage/CEN65/src
  (`plp-agrespd.f`, `defprbpd.f:810`, `osicallsc.cpp`).

## Working rules (project conventions that bit before)

- Docs: GFM, ATX headers, 80-char lines; LaTeX math per
  `docs/formulation/mathematical-formulation.md` conventions; update
  `docs/methods/sddp.md` in lockstep (stale §3.3/§4.5/§4.6 noted above).
- Build: g++-15 default; scratch dir via
  `BUILD_DIR=$(bash tools/mk_scratch_build.sh)` (never the repo `build/`,
  never /tmp directly); CIFast; single foreground build, batch edits, build
  once; tests always `ctest -j20` (never serial), under `nice -n 19`, with
  `ulimit -c unlimited`.
- Solver: CPLEX for gtopt work; gate MIP-dependent tests on
  `has_mip_solver()`; license flakes → `run_or_skip_license` helpers in
  `solver_test_helpers.hpp`.
- Tests: doctest, `<doctest/doctest.h>` first, `using namespace gtopt;` at
  file scope (no NOLINT), `doctest::Approx` for doubles, trailing commas in
  brace-init lists, unique outer namespace per test file (unity builds).
  Never `NOLINT(bugprone-unchecked-optional-access)` — use `value_or` /
  `(opt && *opt == x)`.
- Never `std::views::iota` — use `iota_range` from `gtopt/utils.hpp`.
- SDDP facts already settled (do not relitigate): `cut_sharing`
  accumulate/expected/max are invalid for distinct-sample-path scenes
  (keep WARN-only); gtopt does NOT warm-start backward resolves; the
  ~$931M FCF obj_constant is never the explanation for a MIP gap.

Start by finishing the unread list, then write deliverable 1. Keep a
findings ledger as you read: every eps-tolerance, every place uniform
probability is assumed, every claim in code comments that the doc must
either prove or refute (two have already been found wrong historically —
the block_ecost cut-sharing justification and the multicut 1/N comment's
silent uniformity assumption).
