# FutureCost / UserModel element refactor + AMPL indexing-set scope

Status: design agreed (2026-06-21).  **Landed:** schema
`include/gtopt/future_cost.hpp` (typed enums); **piece 1** — the capability
concepts (`HasAddToLp`/`HasAddToOutput` split + the new
`HasAddToPhaseLp`/`HasAddToGlobalLp`/`HasAddToPlanning`) and the
`add_to_planning_lp` driver pass (run once per (scene, phase) cell after the
stage loop; inert until an element provides the hooks); **piece 2A** —
`FutureCostLP` + `json_future_cost.hpp` wired end-to-end (inert), in
`collections_t` after `UserConstraintLP`; **piece 2B** — sharpened the
`mean_shift` bound-consistency safety net (`test_sddp_boundary_cuts_mean_shift`).
**Pending:** piece 2 C/D (α output), pieces 3–5.

### Piece 2 D — RE-SCOPED to "α output only" (decided 2026-06-21)

The doc's original "move `register_alpha_variables` + cut-load + rebase into
`add_to_global_lp`" does **not** fit: `add_to_global_lp` is per-(scene,phase)
cell at LP-build time, but `register_alpha_variables` is per-scene
(`PlanningLP`, all phases) at solve-time, and the cut-load must run after
`save_base_numrows()`.  So D is re-scoped to OUTPUT ONLY — the proven
α/cut/rebase machinery stays put; `FutureCostLP` just saves α to the solution.

**STATUS: IMPLEMENTED (2026-06-21).**  Saves the single α (`alpha`) + every
multicut `varphi_s` (as `alpha_<s>`, source-scene order) + the per-scene rebase
c̄ (`rebase`) to the solution — `add_col_sol` for the α columns (read at write
time) + `add_col_sol_values` for c̄, at each cell's terminal block.  Population
is `SDDPMethod::populate_future_cost_output()`, called at the END of `solve()`
(both sync + async returns) — NOT in `initialize_solver` (the iterations reset
the per-cell FutureCostLP).  `FutureCostLP` gained a no-op `update_lp` to stay
resident.  Tests: `test_future_cost_alpha_output.cpp` (α saved) + piece-2B
mean_shift bounds unchanged.  **CONSTRAINT:** needs `low_memory = off`.  Under
`compress` the per-cell FutureCostLP is rebuilt for write_out, dropping the
installed handles; a one-time WARN fires (non-silent).  Compress support — a
per-cell apply in the simulation-pass write path — is the documented follow-up.

**α-output as-built notes (C1–C3):**
- α is a **scene-phase-context** column (one per `(scene, phase)`, cost `1/N`),
  registered in the state-var registry — `register_alpha_variables`
  (`sddp_method_alpha.cpp:115-145`), located via
  `find_alpha_state_var(sim, scene, phase)->col()`.  It is NOT block-context, so
  it can't reuse `DecisionVariableLP::add_to_output` directly.
- **C1** `future_cost_lp.{hpp,cpp}`: store the solved α + c̄ per cell;
  `add_to_output` emits `FutureCost/{alpha,rebase,approx_fcf=α+c̄}` as VALUES via
  `OutputContext::add_col_sol_values` (raw doubles, `output_context.hpp:178`) at
  the terminal block — sidesteps the scene-phase-column output path.  **REQUIRED
  (2026-06-21): save BOTH layouts — the single α AND, under multicut, every
  per-source-scene `varphi_s` (N columns, uid `sddp_alpha_uid + s`).  Store them
  keyed by source scene and emit each (`FutureCost/alpha` for the single layout,
  `FutureCost/varphi_<s>` / `alpha_<s>` for the multicut layout), never just
  `varphi_0`.**
- **C2** `sddp_method.cpp`: a post-solve per-cell hook reads `scene_alpha_offset`
  + the α value(s): `li.get_col_sol()[alpha_col]` for the single α, OR — under
  multicut — loops `s = 0..N-1` via
  `find_alpha_state_var(sim, scene, phase, source_scene_s)`
  (`sddp_types.hpp:1123`) reading every `varphi_s`; sets them all (+ c̄) on that
  cell's `FutureCostLP` (read-only w.r.t. the LP).
- **C3** verify: piece-2B (`mean_shift`) bounds unchanged + new test asserts
  `FutureCost/{alpha,rebase}` appear in an SDDP solution.

## Motivation

The FCF / α (cost-to-go) and the boundary cuts that linearise it are built
by routines scattered across the SDDP units (`sddp_method_alpha.cpp`,
`sddp_boundary_cuts.cpp`, `sddp_cut_io.cpp`) and pulled into both
`monolithic_method.cpp` and `sddp_method` with options threaded through
in a dispersed way (`boundary_cuts_file`, `boundary_cuts_mean_shift`,
`scale_alpha`, `boundary_cut_sharing`, `boundary_cuts_mode`). Two concrete
gaps surfaced:

1. **α is never saved.** It's a real LP column (`register_alpha_variables`
   → `varphi_s`, priced `1/N`) whose optimal value is the realised
   cost-to-go (≈ −$25.8M on the CEN 2025-10-05 case), plus the rebase
   constant `c̄`. Neither is written to the solution — only recoverable
   indirectly from the log.
2. **The shared FCF code lives under an SDDP-specific name** yet is used
   by the monolithic method too — an organisation smell.

Separately, the AMPL layer (`UserConstraint`, `DecisionVariable`) only
instantiates **per-block**, so the FCF cut — which must be a single
terminal row on a scalar α — had to be faked with ad-hoc
`DecisionVariable.scope` (single last-block) and `UserConstraint.daily_sum`.
Those are hand-rolled special cases of AMPL's native **indexing sets**.

## Four pieces (implementation order)

### 1. Capability concepts (relax the contract from gate → discriminator)

`system_lp.hpp` defines `concept AddToLP` requiring **both** `add_to_lp`
and `add_to_output`, and `static_assert`s it on every active collection.
The visitor already skips passive elements (`if constexpr (!AddToLP<T>)`),
so the discriminator idiom exists — but the concept is coupled.

- **Key finding (verified):** the existing `add_to_lp` is *already
  multi-scope*. `StorageBase::add_to_lp` builds the per-block balance
  (block), the within-stage storage balance + `eini`/`efin` (stage), AND
  the `efin` StateVariable + cross-phase link via
  `add_state_variable`/`defer_state_link` (phase). So **do not rename**
  `add_to_lp` — it is not block-only, and renaming would misdescribe it
  and churn ~40 files for nothing.
- Keep `add_to_lp` as each element's combined operational build
  (`HasAddToLp` = "builds its full operational LP"). The piece-1 split
  (`HasAddToLp`/`HasAddToOutput` + visitor discrimination) **landed and
  compiles** — correct as-is, no rename.
- **Final granularity taxonomy (verified against Generator/Reservoir/
  Turbine):** the STAGE is the natural unit — every current element does
  per-stage pre-compute (CapacityBase, AMPL metadata, fuel/conversion
  resolution, junction lookup) then an internal block loop. None is
  purely per-block (even Turbine has stage setup). So:
  - `add_to_stage_lp` (= today's `add_to_lp`, renamed for accuracy):
    the DEFAULT — **all** current elements live here.
    `HasAddToLp`→`HasAddToStageLp`. Mechanical, **deferrable** (functionally
    `add_to_lp` already is this).
  - `add_to_block_lp`: RESERVED, no current occupant — a future thin
    per-block element only.
  - `add_to_phase_lp(sc, scene, phase, planning_lp)`: new state-var /
    recourse constructs (reuses generic `add_state_variable`/
    `defer_state_link`).
  - `add_to_global_lp(planning_lp)`: `FutureCost`, annual caps.
- Value is in adding the coarse `add_to_phase_lp`/`add_to_global_lp`
  passes (`scene×phase`, once) — dispatched via
  `if constexpr (HasAddTo<Scope>Lp<T>)`; existing elements untouched.
  AMPL stage/week/month budgets are handled *inside* `add_to_stage_lp`
  by generalising `daily_sum` to a scope/aggregation field (piece 4), not
  by a separate per-stage pass.

**Migration staging (least changes — verified: no element is purely
per-block; all resolve junctions/buses/capacity/expression per stage):**
1. *This stage:* rename `add_to_lp`→`add_to_stage_lp`,
   `HasAddToLp`→`HasAddToStageLp` across all ~40 elements (mechanical,
   zero behavior change); add the `add_to_phase_lp`/`add_to_global_lp`
   hooks (for `FutureCost`). Move **nothing physical** to
   `add_to_block_lp`; reserve it.
2. *Piece 4:* the AMPL construct (`UserConstraint`/`DecisionVariable`) is
   the first occupant of `add_to_block_lp` — its per-stage setup stays in
   `add_to_stage_lp`, its per-block row instantiation becomes
   `add_to_block_lp`, and `scope: stage|phase|global` route to the coarse
   methods. Engine elements are never block-split.
- Turn the blanket `static_assert(AddToLP<…>)` into per-capability sanity
  checks on intended-active types only.
- Genuinely-passive elements then drop their no-op `add_to_*` and are
  auto-skipped; new data-only collections need no methods at all.

### 2. `FutureCost` element (planning-level)

`FutureCost` (schema, done) + `FutureCostLP` satisfying
`HasAddToPlanning` + `HasAddToOutput`:

- `add_to_lp(planning_lp,…)` absorbs `register_alpha_variables` +
  `load_boundary_cuts_csv` + the α-rebase (`mean_shift`, anchored at the
  reservoirs' `efin` targets) + `scale_alpha`. Stores α col handles per
  (scene,phase) and the per-scene `c̄`.
- `add_to_output(out)` writes, per scene (mapped scene→scenario/stage at
  the terminal block via `add_col_sol_values`):
  `FutureCost/alpha` (varphi_s), `FutureCost/rebase` (c̄),
  `FutureCost/value` (un-rebased FCF = α + c̄).
- Consolidated options live on the element: `cuts_file`, `scale_alpha`,
  `mean_shift`, `sharing`, `mode`, `valuation`.
- `monolithic_method` / `sddp_method` stop calling the three routines and
  just ensure the element is in the system. SDDP runtime cut generation /
  cross-scene sharing stays SDDP-side, appending onto the same α columns.

### 3. `UserModel` element (generic AMPL capture)

Generalises `UserConstraint` (rows) + `DecisionVariable` (cols) into one
element that owns **all** LP structure the AMPL layer injects — including
auxiliary cols/rows that aren't first-class elements today.

- `add_to_lp` registers/tags every user col (`ColIndex` by name) and row
  (`RowIndex` by name).
- `add_to_output` emits `add_col_sol`/`add_col_cost` per col and
  `add_row_dual`(+slack) per row → `output/UserModel/<tag>/{sol,cost,dual,slack}`.
- Also gains `HasAddToPlanning` for **global/per-phase** user constructs
  (see piece 4), e.g. a user-supplied FCF.

### 4. AMPL indexing-set scope + `sum` aggregation

Adopt AMPL's native semantics (this is core AMPL, not an extension):
the index set on a declaration *is* the scope.

- `UserConstraint` / `DecisionVariable` gain an explicit **scope/index**:
  `block` (default), `stage`/`phase`, or `global` (un-indexed → one row/col).
  `daily_sum` and single-block `scope` become special cases.
- Provide a `sum{…}` (and `prod`/`max`/`min`) aggregator over finer
  indices (block→stage, block/stage→global) so coarse-scope constraints
  reach per-block variables.
- `global`/`stage`-scoped objects are built via `HasAddToPlanning`
  (planning-level), not per-block.

This yields the **user-overridable FCF** for free: `var alpha` (global
DecisionVariable) + `s.t. FcfCut: alpha >= rhs - sum{r in RES} wv[r]*vol_end[r]`
(global UserConstraint), plus a flag to disable the built-in `FutureCost`.
`FutureCost` is then simply the built-in instance of that pattern.

### 5. AMPL state variables + `AmplFutureCost` (user SDDP recourse)

The SDDP state/cut machinery is already **generic**, so this is exposure,
not new infrastructure:
- cut generation iterates `sim.state_variables(scene,phase)` (not a fixed
  set) — any registered state var is in the optimality/feasibility cuts;
- `add_state_variable(Key,col,…)` + `defer_state_link(prev_key,here_col)`
  already provide registration + inter-stage coupling generically
  (reservoir storage, capacity, α all use them);
- `SystemContext::add_ampl_variable` is already a hook.

Additions:
- **AMPL `state` variable.** A `DecisionVariable`/`UserModel` var gains
  `state: true` + a `link` (this phase's start = previous phase's end).
  Its `add_to_lp` calls `add_state_variable` + `defer_state_link`; it then
  rides the generic backward pass — coupled across phases and present in
  every cut. Must be `stage`/`global`-scoped (piece 4), never per-block.
- **`AmplFutureCost`.** *Built on the `FutureCost` element, not a parallel
  path.* The AMPL author declares a global α (`state`/cost
  DecisionVariable) + user cuts (global `UserConstraint`s over α and the
  user's state vars); these **feed a `FutureCost` instance**, reusing its
  `add_to_lp`/`add_to_output`/α-registration/rebase — same element, sourced
  from AMPL instead of `boundary_cuts.csv`. A flag selects user vs
  built-in cuts (or disables the built-in entirely so the user's recourse
  replaces ours). Because cuts are generic over state vars, the user α +
  cuts integrate with the existing forward/backward passes with no engine
  changes. So `FutureCost` is the single FCF engine; `boundary_cuts.csv`
  and `AmplFutureCost` are just two front-ends that populate it.

This makes the AMPL layer a full **multi-stage stochastic** modelling
surface: per-block operating constraints (piece 4), coarse-scope
budgets/agreements (piece 4), and now cross-stage **state + recourse**
(piece 5) — all author-able without C++.

## Capability matrix

| element | HasAddToLp | HasAddToOutput | HasAddToPlanning |
|---|:--:|:--:|:--:|
| Generator / Reservoir / Line / … | ✓ | ✓ | |
| FutureCost | | ✓ | ✓ |
| UserModel | ✓ | ✓ | ✓ (for global/phase user objects) |
| Fuel-as-data / reference-only | | | |

## Files

- new: `future_cost.hpp` (done), `json/json_future_cost.hpp`,
  `future_cost_lp.{hpp,cpp}`; `user_model.hpp`,
  `json/json_user_model.hpp`, `user_model_lp.{hpp,cpp}`.
- edit: `system_lp.{hpp,cpp}` (capability concepts + visitors),
  system schema (add `future_cost`, `user_model`), `monolithic_method.cpp`
  + `sddp_method*` (use the element), `user_constraint.{hpp}` /
  `decision_variable.hpp` (scope), the AMPL resolver (`sum` aggregation),
  CMake auto-discovery picks up new sources.

## Review guards (LP/SDDP + C++26 experts, 2026-06-21 — MANDATORY for pieces 2–5)

**Naming/concept (apply before FutureCostLP):**
- Define a distinct `HasAddToPlanning` concept; name planning methods
  `add_to_phase_lp` / `add_to_global_lp` (NOT `add_to_lp(planning_lp)`),
  so a signature typo can't silently no-build the FCF.
- Add per-type `static_assert(HasAddToPlanning<FutureCostLP>)` /
  `HasAddToOutput<...>` (and for UserModelLP) — the discriminator's safety net.
- Schema enums: `sharing`/`mode`/`valuation` → typed enums
  (`CutSharingMode`, …), not `OptName` (no-magic-strings).
- `collections_t`: FutureCostLP after ReservoirLP (+ after UserConstraintLP) —
  enforce with a `type_index_in` static_assert; planning pass runs after stage.

**Numerical correctness (P0):**
- **c̄ LB/UB corrections** must move atomically: rewire every
  `scene_alpha_offset()` site (`sddp_method_iteration.cpp:1134,1653`,
  `sddp_iteration.cpp:1577`) to `FutureCostLP::scene_alpha_offset()`. Add a
  bound-consistency regression test FIRST (LB/UB with mean_shift on must equal
  off + Σ c̄; gap not spuriously ~0).
- α output via physical `li.get_col_sol()` (ScaledView), never raw;
  `value = α'_phys + c̄_phys`; assert `|value−(α'+c̄)|<1`.
- Migrate the single-cut equality + α-free (`set_col_low_raw(-inf)`) as ONE
  atomic pair (`sddp_boundary_cuts.cpp:892-913`).
- AMPL `state:true` without `link` ⇒ ERROR at `add_to_phase_lp`.
- Guard dual α-registration (built-in `FutureCost` vs `AmplFutureCost`):
  decide mutually-exclusive + runtime check.
- Preserve aperture-system α mirroring (call `register_alpha_variables`
  verbatim, don't re-inline); set `var_scale` on user state vars.

**Clarity (P2):** rename output `FutureCost/value` → `FutureCost/approx_fcf`
(it is the LP lower-bound FCF approximation, exact only at the efin target).

## AMPL-resolver guards (irrigation/AMPL-framework expert, 2026-06-21 — pieces 3–5)

> Bottom line: "exposure, not new infrastructure" was overstated for pieces 4–5.
> The state/cut *primitives* exist; the AMPL→state bridge + coarse-scope driver
> passes do NOT. Piece 5 is blocked on piece 1.

**Piece 4 — scope + `sum{}` (P0):**
- **`sum{}` is a NEW operator, not an overload.** Today's `sum(...)`
  (`SumElementRef`, `element_column_resolver.cpp:693`) aggregates over *elements*
  of one type at the ambient `(scenario,stage,block)`. The proposed
  `sum{b in stage}` aggregates over *time*. Introduce a new AST node
  (`TimeAggRef`) + a block/stage-iterating resolver (per-block `find_ampl_col`
  exists, `system_context.hpp:602`); do NOT route through `collect_sum_cols`.
- **`global`-scope rows have no `LpContext` variant** (`lp_context.hpp:85`:
  Stage/Block/ScenePhase/monostate). A `monostate` global row risks colliding in
  `add_row` dedup (`user_constraint_lp.cpp:216`). Add a `GlobalContext`/
  `PhaseContext` variant (or document `ScenePhaseContext` for rows) BEFORE landing.
- Keep `scope` (granularity reduction) distinct from the existing `for(...)`
  clause (instance restriction) — orthogonal; author `scope` as a typed JSON enum
  field (`block|stage|phase|global`), not an in-expression keyword
  (`decision_variable.hpp:102` precedent).

**Piece 4 — units / subsumption (P1):**
- `sum{}` over *rate* vars (flow [m³/s]) MUST carry `dur[b]` weighting or it
  misprices (the `feedback_stage_avg_flows` class); spell it `sum{b} dur[b]*flow[b]`.
- `daily_sum` is subsumed ONLY if the index set supports a sub-stage `day`
  window AND a `weight: duration|count` modifier (energy-vs-count is load-bearing
  for PLEXOS `RHS Day`). Do NOT silently retire `daily_sum`
  (`user_constraint_lp.cpp:1009,1063`).
- Two `sum` spellings (`sum(...)` element vs `sum{...}` time) — document loudly
  in the `constraint_expr.hpp:90` grammar block.

**Irrigation — reuse vs keep (no conflict, no merge):**
- `RightBoundRule` (dynamic *bounds*, `right_bound_rule.hpp`) and `VolumeRight`
  reset/`skip_state_link` re-provisioning (`volume_right_lp.cpp:172-228`) are a
  different layer than constraint-row generation — **keep them**.
- **Trap:** do NOT re-author Maule/Laja monthly modulation via AMPL scope+`sum{}`;
  it stays on `RightBoundRule.stage_month`. Avoid two mechanisms for one physics.
- Laja partition / Maule 20-80 are already plain block-scoped `UserConstraint`s —
  no scope/`sum{}` needed (existing happy path).

**Piece 5 — AMPL state vars (P0, blocked on piece 1):**
- `add_ampl_state_variable` does NOT exist; `state_wrapped` is set by the parser
  (`constraint_parser.cpp:1407`) but UNCHECKED in `build_row_from_terms`
  (`user_constraint_lp.cpp:421`) — `state(...)` is currently a typing no-op.
  Build the bridge: schema `state:true`/`link` → `add_state_col` +
  `defer_state_link` (`system_context.hpp:374,512`).
- Coarse-scope state vars need the `add_to_phase_lp`/`add_to_global_lp` passes,
  which the driver lacks (`system_lp.cpp:392` is stage-only). The new
  once-per-(scene×phase) pass is the largest net-new mechanism → **piece 1 first**.
- AMPL `link` must synthesize a previous-phase `StateVariable::Key` from the var's
  own identity; give user state vars a dedicated `class_name` key prefix so cut I/O
  round-trips without colliding with engine state (reservoir efin, α).

**Piece 3 — UserModel (P1):**
- Reuse `add_ampl_variable` routing + honor `silent_flatten_pass()`
  (`system_context.hpp:316-356,530`) — do NOT add a parallel registry (double-
  registers on SDDP rebuild).
- Decide the aux-col policy: abs/min/max lowering creates `add_aux_col` columns
  (`user_constraint_lp.cpp:262`) NOT registered in the AMPL registry; "capture
  all" must either register them or document them as internal/non-captured.

## Risks / notes

- α-bootstrap pin (`lowb=uppb=0` until first cut) preserved in `add_to_lp`.
- `scale_alpha=1e5` readback: `add_col_sol` already inverts column scale.
- aperture-system mirroring of α retained.
- Ordering: FutureCost built after reservoirs/state-vars exist (cut
  references their terminal cols; rebase reads `efin`).
