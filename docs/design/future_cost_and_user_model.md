# FutureCost / UserModel element refactor + AMPL indexing-set scope

Status: design agreed (2026-06-21).  **Landed:** schema
`include/gtopt/future_cost.hpp` (typed enums); **piece 1** — the capability
concepts (`HasAddToLp`/`HasAddToOutput` split + the new
`HasAddToPhaseLp`/`HasAddToGlobalLp`/`HasAddToPlanning`) and the
`add_to_planning_lp` driver pass (run once per (scene, phase) cell after the
stage loop; inert until an element provides the hooks); **piece 2A** —
`FutureCostLP` + `json_future_cost.hpp` wired end-to-end (inert), in
`collections_t` after `UserConstraintLP`; **piece 2B** — sharpened the
`mean_shift` bound-consistency safety net (`test_sddp_boundary_cuts_mean_shift`);
**piece 2 C/D** — α output (COMPLETE, see below); **piece 3** — `UserModel`
element (COMPLETE, see §3); **piece 4** — AMPL indexing-set scope + `sum{}`
time-aggregation (COMPLETE, see §4); **piece 5 step 1** — AMPL `state`-variable
bridge (COMPLETE, see §5); **piece 5 step 2a** — static user-overridable FCF
(`use_user_alpha` / `user_alpha_uid` + built-in-α suppression + guards +
output, COMPLETE, see §5); **piece 5 step 2b** — `FutureCost` boundary config
consolidation (COMPLETE, see §5); **piece 5 step 2c Increment A** — dynamic
`AmplFutureCost` / SDDP cross-phase recourse over a user α (single-cut, single +
multi scene, NO aperture; COMPLETE, see §5 "STATUS — step 2c Increment A").
**Deferred:** step 2c Increment C (aperture) + Increment D (multicut, rejected
at init).

### Piece 2 D — RE-SCOPED to "α output only" (decided 2026-06-21)

The doc's original "move `register_alpha_variables` + cut-load + rebase into
`add_to_global_lp`" does **not** fit: `add_to_global_lp` is per-(scene,phase)
cell at LP-build time, but `register_alpha_variables` is per-scene
(`PlanningLP`, all phases) at solve-time, and the cut-load must run after
`save_base_numrows()`.  So D is re-scoped to OUTPUT ONLY — the proven
α/cut/rebase machinery stays put; `FutureCostLP` just saves α to the solution.

**STATUS: COMPLETE (2026-06-21) — works under ALL `low_memory` modes.**  Saves
the single α (`alpha`) + every multicut `varphi_s` (as `alpha_<s>`, source-scene
order) + the per-scene rebase c̄ (`rebase`) to the solution at each cell's
terminal block.  `FutureCostLP::add_to_output` **self-finds** its data at write
time: the α columns from the persistent `SimulationLP` state-variable registry
(`alpha_cols_on_cell`, via `OutputContext::system_context()`) + c̄ from
`SimulationLP::alpha_offset(scene)`.  Both registries outlive the per-cell LP
rebuild that `write_out` performs under `compress`, so NO per-cell resident
stash is needed (an earlier end-of-solve populate-onto-FutureCostLP only worked
under `off`).  `SDDPMethod::populate_future_cost_output()` (end of `solve()`,
sync+async) now just publishes the per-scene c̄ into `SimulationLP`.  The α /
cut / rebase machinery is untouched — read-only, SDDP bounds unchanged.  Tests:
`test_future_cost_alpha_output.cpp` (α data available post-solve under BOTH
`off` and `compress`) + piece-2B mean_shift bounds unchanged.  Needs a
`FutureCost` element in the model (`future_cost_array`) to route the output.

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

**STATUS: COMPLETE (2026-06-22).**  `UserModel` (schema +
`json_user_model.hpp`) bundles a `tag`, a `variable_array`
(`Array<DecisionVariable>`) and a `constraint_array`
(`Array<UserConstraint>`) into one element.  `UserModelLP` REUSES the
existing machinery rather than forking it: it holds one internal
`DecisionVariableLP` per declared var and one internal `UserConstraintLP`
per declared constraint, and drives their `add_to_lp` /
`add_to_phase_lp` / `add_to_global_lp` verbatim — so the shared
expr resolver, the `add_ampl_variable` routing, the `silent_flatten_pass()`
gate (no double-register on SDDP rebuild), the soft-slack folding, the
scope routing (`block|stage|phase|global`) and the aux-col lowering are
all the SAME code paths as the standalone elements.  `add_to_output` does
NOT delegate (that would file under `DecisionVariable/` / `UserConstraint/`);
instead it reads each delegate's col/row holders through new ADDITIVE
read-only getters and emits them under `output/UserModel/<tag>/<name>_{sol,
cost,dual,slack}` via the standard `OutputContext` calls.  Wired into
`collections_t` after `UserConstraintLP` (before `FutureCostLP`), with
`static_assert(HasAddToLp && HasAddToOutput && HasAddToPlanning<UserModelLP>)`
and `lp_type_index_v` ordering asserts.  Tests:
`test_user_model.cpp` (schema round-trip; one-var-one-constraint build;
output capture under the tag; global-scope single-row no-collision; aux-col
policy).  `UserConstraint` / `DecisionVariable` are UNCHANGED (irrigation
keeps working).

**Aux-col policy (P1) — DECIDED.**  `abs(x)` / `min` / `max` lowering in
`user_constraint_lp.cpp` creates `add_aux_col` columns (`abs_aux` /
`min_aux` / `max_aux`) that are NOT registered in the AMPL variable
registry and have no stable user-facing name.  `UserModel` captures ONLY
the NAMED user cols (`DecisionVariable.value`) and NAMED rows
(`UserConstraint.constraint` + its slacks); the aux cols/rows are treated
as **internal / non-captured** LP-implementation detail (the simplest
correct policy — they have no tag-name to file under, and their physical
content is fully recoverable from the named row's dual).  This decision is
also stated in a comment in `source/user_model_lp.cpp`.

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

**STATUS — step 1 (AMPL `state`-variable bridge): COMPLETE (2026-06-22).**
`DecisionVariable` gained `state: true` + `link: true` (schema +
`json_decision_variable.hpp`).  `DecisionVariableLP` now honours `scope`:
`block` (legacy per-block) and `stage` (one col per (scenario, stage)) build
in the operational `add_to_lp`; `phase` / `global` build ONE column per
(scene, phase) cell in the NEW planning passes
(`DecisionVariableLP::add_to_phase_lp` / `add_to_global_lp`, wired into the
piece-1 `add_to_planning_lp` driver).  When `state: true`, the coarse cell
column is registered as a cross-phase `StateVariable` via
`SystemContext::add_state_col` and (`link: true`) deferred-linked to the
previous phase's same-variable column via `defer_state_link` — so it RIDES
the existing generic backward pass (`sim.state_variables(scene,phase)`),
coupled across phases and present in every optimality / feasibility cut, with
NO engine change.  The bridge **fixes the typing no-op**: previously
`state: true` did nothing; now it produces a real `StateVariable`.

Guards (all enforced + tested):
- `state: true` WITHOUT `link` ⇒ hard error at construction (guard 2).
- `state: true` with `block` scope ⇒ hard error (guard 3) — a state var has
  no single end-of-phase value to propagate.
- the `StateVariable::Key::class_name` uses a DEDICATED prefix
  (`DecisionVariableLP::StateClassName = "UserStateVar"`), distinct from the
  element's own `DecisionVariable` class AND from engine state (reservoir
  `efin`, the built-in α `sddp_alpha_lp_class`), so its identity round-trips
  through cut I/O without colliding (guard 4).
- `var_scale = 1.0` on the user state column (guard 6 — plain LP column, no
  semantic scale).
- ADDITIVE: the legacy block/stage `DecisionVariable` build is byte-for-byte
  unchanged; the built-in boundary-cut FCF / reservoir efin / α paths are
  untouched (pieces 2/4 tests stay green).

`UserModelLP` now drives its bundled variables' planning passes (variables
before constraints in `add_to_phase_lp` / `add_to_global_lp`, mirroring the
standalone collection order) so a bundled `state` var + a `global`
`UserConstraint` over it builds in the right order — the foundation for
`AmplFutureCost`.

Tests: `test/source/test_ampl_state.cpp` — schema round-trip; both guard
errors; registration under `UserStateVar` in every phase (never under
`DecisionVariable`); cross-phase dependent-variable links (last phase has
none); SDDP solve converges with the user state var present + coupled
(`links=2/2` in the backward log).

**STATUS — step 2a (static user-overridable FCF): COMPLETE (2026-06-22).**
A `FutureCost` element can now carry `use_user_alpha: true` + `user_alpha_uid`
(typed `Uid`), sourcing the cost-to-go from a user-authored α
`DecisionVariable` + `UserConstraint` cuts instead of the built-in
boundary-cut α (schema + JSON round-trip in `future_cost.hpp` /
`json_future_cost.hpp`).

- **Built-in-α suppression.**  `register_alpha_variables` gained a trailing
  `register_as_state_variable = true` param (default = byte-for-byte legacy).
  When `false` the α column is still ADDED to each LP (layout / aperture
  mirroring unchanged) but priced `0` and NOT registered as a
  `StateVariable`, so `find_alpha_state_var` returns null and the cut router /
  `apply_alpha_floor` / `bound_alpha` never touch it — the built-in α is
  inert (pinned `lowb = uppb = 0`).
  `SDDPMethod::initialize_alpha_variables` passes `!has_active_use_user_alpha`.
  The `register_alpha_variables` BODY is otherwise untouched (the call is
  gated / parameterised, not re-inlined).
- **Runtime guards** in `SDDPMethod::initialize_solver`, all returning
  `std::unexpected` (NOT throw): (1) `use_user_alpha` + `boundary_cuts_file`
  are mutually exclusive; (2) `user_alpha_uid` is required AND must match a
  `DecisionVariable` with `cost != 0` (an unpriced α leaves the master
  unbounded below in the future-cost dimension); (3) `use_user_alpha` skips
  the whole boundary-cut load block (`m_scene_alpha_offsets_` stays zero);
  `use_user_alpha && mean_shift` ⇒ WARN ignored.
- **Element reads are compress-safe.**  `active_future_cost(planning_lp)` /
  `has_active_use_user_alpha` read the `FutureCost` straight from the System
  INPUT data (`future_cost_array`), NOT the LP collection — the SDDP default
  `low_memory = compress` drops the planning-only `FutureCostLP` collection
  (it has no `update_lp`), leaving `elements<FutureCostLP>()` empty while the
  input array is intact.  Same for the user-α-cost guard
  (`decision_variable_array`).
- **Output.**  `FutureCostLP::add_to_output` emits the user α (located by
  `user_alpha_uid`) as `FutureCost/alpha` under `use_user_alpha`.

**Pricing convention (proven, not hand-waved).**  The built-in α is priced
`1/N` (= 1.0 for the single-scene single-α layout) on a column scaled by
`scale_alpha`, contributing the realised cost-to-go in $.  The equivalent
user α must carry the SAME physical 1:1 weight: a `DecisionVariable` with
`cost = 1.0`, `cost_type = "raw"` (face value — NO probability / discount /
duration weighting), free below (cost-to-go may be negative).  A coef-0
boundary cut `α ≥ rhs` and the user cut
`decision_variable('user_alpha').value >= rhs` then BOTH add `rhs` once to the
objective.  Gold-standard equivalence test (`test_ampl_future_cost.cpp`): a
single-phase monolithic case solved twice (boundary cut vs user α + cut)
gives the SAME objective — `baseline 2850 + rhs 15250 = 18100` on both sides,
within `1e-3`.  Plus: schema round-trip, the mutual-exclusion
`std::unexpected`, the cost==0 `std::unexpected`.

**STATUS — step 2b (boundary config consolidation): COMPLETE (2026-06-22).**
`SDDPBoundaryConfig` + `boundary_config(const FutureCost&)` map the element's
authored boundary fields (`cuts_file`, `scale_alpha`, `mean_shift`, `sharing`,
`mode`) onto the resolved `SDDPOptions` boundary fields.
`SDDPMethod::initialize_solver` applies the override early (before the
scale_alpha auto-derivation + the boundary-cut load), gated on the element
having EXPLICITLY set each field — read-site only, byte-for-byte unchanged
when there is no `FutureCost` element or it leaves a field unset.  Test:
element `mean_shift=false` beats `SDDPOptions.boundary_cuts_mean_shift=true`
(`scene_alpha_offset == 0`); `test_sddp_boundary_cuts_mean_shift` stays green.

**STATUS — step 2c Increment A (dynamic AmplFutureCost / SDDP cross-phase
recourse): COMPLETE (2026-06-22) — single-cut, single + multi scene, NO
aperture.**  The user α is now a first-class SDDP backward-pass recourse column:
the modeller authors a global `state`/`link` α `DecisionVariable` (cost 1.0
raw, free below) + global `UserConstraint` cuts over it, and the SDDP backward
pass refines its value function in every phase exactly as it does the built-in
α.  Every edit is gated on `has_active_use_user_alpha(planning_lp())`
(compress-safe — reads the System INPUT `future_cost_array`), so the default
path is **byte-for-byte unchanged** (all `ieee_*`/`c0`/`juan` cases have no
`FutureCost`; `test_sddp_boundary_cuts_mean_shift` has one but leaves
`use_user_alpha` unset → guard false).

As-built (six surgical edits, each gated false on the default path):

- **`find_user_alpha_state_var`** (`sddp_method.cpp`): resolves the user α
  (class `DecisionVariableLP::StateClassName` = "UserStateVar", col_name
  "value", `uid == user_alpha_uid`) by scanning the cell's
  `(scene, phase, kind)` state-variable map — NOT a keyed lookup, because the
  user α was registered with concrete scenario/stage uids the SDDP layer does
  not know (whereas the built-in α uses the default `unknown` uids).
- **`collect_state_variable_links`** (`sddp_method_alpha.cpp`): skips the
  active user α from `outgoing_links` (it is the cost-to-go ESTIMATOR, not a
  forward stock — must not get elastic slacks), mirroring the built-in-α skip.
  Only the *active* user α is skipped; any other `UserStateVar` forward state
  still enters `outgoing_links` (verified: `test_ampl_state` "SDDP solve
  converges" keeps `links=2/2`).
- **Bootstrap pin** (`initialize_alpha_variables`): the user α is pinned
  `[0, 0]` on every NON-terminal phase (so the priced free-below α can't drive
  the iter-0 master unbounded), and RELEASED on the TERMINAL phase — its
  bounding `UserConstraint` is a structural row present from LP-build time, so
  freeing it there is the exact analogue of `load_boundary_cuts` releasing the
  terminal built-in α at load time.  Recorded into the compress-replay channel
  via `record_col_bounds_dynamic` (`set_col_*_raw` → pending-col-bounds, keyed
  by column index, replayed by `apply_post_load_replay`).
- **`bound_user_alpha` / `bound_alpha_for_cut` extension**
  (`sddp_method_alpha.cpp`): when the user α is active, a backward optimality
  cut that references it releases its pin on the previous phase (the user α is a
  single column — NOT N `varphi_s`; no cut-derived floor projection, the user's
  own cuts bound it).
- **Backward-pass dispatch** (`sddp_method_iteration.cpp`): `src_alpha_svar`
  resolves to the user α via `find_user_alpha_state_var` when active, so the
  Benders cut's `+1` lands on the user α column — removes the iter-0 SIGABRT.
- **Forward-pass UB estimator** (`sddp_forward_pass.cpp`): strips the user α's
  `col_sol()` directly (var_scale = 1 → raw is already physical, NO
  `× scale_alpha`) so the realised UB excludes the cost-to-go consistently with
  the built-in path.
- **Multicut guard** (`initialize_solver`): `use_user_alpha && cut_sharing ==
  multicut` → `std::unexpected` (the user α is one column, not N `varphi_s`).

**Proof (`test_ampl_future_cost.cpp`, "AmplFutureCost 2c — user α drives SDDP
recourse to the analytic optimum"):** a 3-phase hydro fixture with a user α +
terminal `UserConstraint` `user_alpha + 90·reservoir('rsv1').efin ≥ 45000,
for(stage in {3})` + `use_user_alpha` converges (gap_only, stationary off) to
LB == UB == $338850 — the INDEPENDENTLY hand-derived deterministic-equivalent
optimum (terminal efin forced to the unique emax=500 corner; thermal 6730 MWh ·
$50 + hydro 470 MWh · $5 + α=0).  The backward log shows `links=1/1` and a
non-trivial α propagated phase-to-phase.  An analytic oracle is used rather than
a `boundary_cuts.csv` cross-run because the boundary-cut path applies
transformations the user-α path deliberately does not (single-cut `≥`→`=`
equality, `mean_shift` rebase, and `apply_alpha_floor`'s worst-case-box
projection that has no off-switch) — so the two are architecturally distinct
formulations whose bounds need not coincide; convergence to the true optimum is
the rigorous, non-circular invariant.

**DEFERRED — Increment C (aperture).**  The equivalence test sets
`aperture_chunk_size = -1` (aperture disabled).  The aperture backward pass
clones the phase LP into a parallel `aperture` `SystemKind` registry and
installs cuts there too (`add_cut_row`'s dual-install block); mirroring the user
α onto the aperture system + dual-installing user-α cuts is not wired.  The
backward dispatch + `bound_alpha_for_cut` helpers already take a `SystemKind`
argument, so the extension is mechanical but untested — deferred until a fixture
exercises it.

**DEFERRED — Increment D (multicut).**  Rejected at `initialize_solver` with
`std::unexpected`.  `CutSharingMode::multicut` expects N dedicated `varphi_s` α
columns (one per source scene) to route per-scenario cuts onto; the user α is a
single priced column.  Supporting it would require N user-α columns (or a
broadcast scheme), which is a larger design change — deferred.

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
