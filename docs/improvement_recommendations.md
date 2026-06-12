# gtopt — improvement recommendations

> **Date:** 2026-05-04
> **Source:** strategic review at the close of the off↔compress symmetry
> investigation (PRs #454 + #455).
> **Author:** Claude Opus 4.7 (1M context)

This document captures the speed / testing / refactoring proposals that
came out of the off↔compress divergence investigation.  Items are ranked
by effort × payoff so they can be planned independently.  Each item
cites the concrete files / lines I touched in this session so the
estimates are grounded in real LOC counts, not hand-waving.

## A. Quick wins (1–3 days each, high payoff, low risk)

### A1. Skip `CPXcopylp` entirely under compress (eliminate the 1.6× overhead) — ~3 days

**What:** Add a `LowMemoryMode::release_in_place` variant.  Instead of
`CPXfreeprob` → `CPXcopylp`, do
`CPXdelrows(base..total)` + `CPXchgbds`/`CPXchgrhs`/`CPXchgcoef` to
revert the existing CPLEX problem to construction-time state.  Keep
`CPXLPptr` alive across "release".  `apply_post_load_replay` then
replays cuts/cols/bounds on the same handle.

**Why:** `CPXcopylp` is the dominant cost in compress mode (1561 calls
× juan run = ~50% of total time).  Skipping it gives compress mode
near-off speed while keeping the snapshot-survives-release invariant.
The matind-sort skip already saved 8.6%; this would knock another ~30%
off compress wall time.

**Risk:** medium — needs careful sequencing of revert calls (delete in
reverse, then reset bounds in chunks) and a CPLEX-state contract test.

### A2. CPLEX warm-start via `CPXcopystart` — ~2 days

**What:** Cache the basis (`CPXgetbase`) at every successful solve.
Pass it back via `CPXcopystart` after each `CPXcopylp` (or
`release_in_place`).  Cheap basis size (~4 bytes/col + 4 bytes/row),
survives compression.

**Why:** Backward-pass resolves on a perturbed LP go from full
barrier+crossover to a few dual simplex iterations — typically 5–20×
faster.  The "no warm-start" memory feedback applied to the *barrier*
method specifically (which doesn't benefit); under simplex/dual
fallback (the natural compress fallback after presolve resets),
warm-start is essentially free perf.

**Risk:** low.  Warm-start is a hint — if invalid, CPLEX silently
re-solves from scratch.

### A3. Aperture filter: hard error on missing aperture UIDs — ~half day

**What:** In `validate_planning.cpp`, walk every `phase.apertures`
list and verify each UID exists in `simulation.aperture_array`.
Fail the validation with `errors.push_back(...)` instead of letting
the runtime "synthetic apertures" path silently drop them.

**Why:** I spent two iterations on the juan aperture run debugging
"why 14/16 feasible" before realizing 2 phase-aperture UIDs (1 and 2)
referenced non-existent aperture entries.  Hard validation catches
this at JSON-parse time with a clear "fix the input" message.

**Risk:** very low — only catches data bugs.

### A4. Promote `GTOPT_DUMP_BACKWARD_LP` to a CLI flag — ~half day

**What:** Add `--lp-dump-backward <dir> [--lp-dump-iter N]
[--lp-dump-phase K-M]`.  Same env-var hook I already have, just
exposed via boost::program_options like the rest.

**Why:** This probe was the smoking gun for the off↔compress
divergence (md5-equal LPs → CPLEX-state asymmetry, not LP-data
asymmetry).  Future debuggers will rediscover this need; making it
discoverable via `--help` saves them 30 minutes.

**Risk:** trivial.

### A5. Off↔compress parity test in CI — ~2 days

**What:** Add a small SDDP fixture (5 phases, 1 reservoir, 1 demand,
1 turbine, 5 stages) under `integration_test/`.  Run both
`low_memory_mode=off` and `low_memory_mode=compress` for 5
iterations; assert the iter-N UB and LB are within ULP-tolerance
(e.g., 1e-6 relative).

**Why:** This entire session's investigation would have been
unnecessary if a CI test had caught `update_lp`'s short-circuit.
Future regressions in the "non-replayed mutation" class will surface
in <30s instead of after months of trajectory drift.

**Risk:** low.  Pick a fixture small enough to run in <10s.

## B. Medium effort (1–2 weeks each)

### B1. Structured per-element "always re-issue" contract — ~1 week

**What:** Introduce a `LpUpdater` base class (or concept) with a
single method `int reissue_lp(SystemLP&, scenario, stage)`.
`ReservoirSeepageLP::update_lp`,
`ReservoirDischargeLimitLP::update_lp`,
`ReservoirProductionFactorLP::update_lp`,
`FlowRightLP::update_lp`, `VolumeRightLP::update_lp` all override it.
The contract is documented once: "must always issue every `set_*`
call regardless of in-memory state".  The base class has a debug-mode
wrapper that detects `state.current_*`-style early-returns.

**Why:** The seepage/DRL bug existed because the contract was implicit
— each element author had to know not to short-circuit.  The pattern
recurs (I saw `flow_right_lp` and `volume_right_lp` had the same
`current_bound` cache; they happened to be correct by accident
because they call `set_col_low/upp` which IS replayed via
`m_pending_col_bounds_`).  Make the contract explicit so the next
element author can't get it wrong.

**Risk:** low.  The 4 existing implementations already follow the
contract — just formalize.

### B2. Split `LinearInterface` into 3–4 classes — ~1.5 weeks

**What:** `LinearInterface` is 2917 + 2800 = 5700 lines.  Mixing
structural LP, solver, lifecycle, cache, label-meta, validation,
scaling.  Refactor into:

* `LpModel` — `add_col`/`add_row`/`set_coeff`/etc. + label-meta + scales (~1500 lines).
* `LpSolver` — `initial_solve`/`resolve`/`is_optimal`/`get_status` (~800 lines).
* `LpLifecycle` — `release_backend`/`reconstruct_backend`/`apply_post_load_replay`/`m_snapshot_`/`m_dynamic_*`/`m_pending_col_bounds_` (~1200 lines).
* `LpCache` — `populate_solution_cache_post_solve`/`invalidate_cached_optimal_on_mutation`/`m_cached_*` (~400 lines).

`LinearInterface` becomes a thin facade that owns the 4 and delegates.

**Why:** New contributors find LinearInterface intimidating.  Bug
investigations had to grep across 5700 lines.  The boundaries above
match the natural failure modes I saw (lifecycle bugs are a separate
class from cache bugs are a separate class from model bugs).

**Risk:** medium.  Many call sites assume direct LinearInterface
access.  Migration via type aliases + member-forwarding macros, then
phased rename.

### B3. Group `SDDPOptions` by concern — ~1 week

**What:** `sddp_types.hpp::SDDPOptions` has 50+ fields covering cut
sharing, apertures, elastic filter, low-memory, log control,
timeouts, recovery.  Group into:

* `SDDPOptions::CutSharing { mode, max_iters, ... }`
* `SDDPOptions::Aperture { count, timeout, save_lp, manual_clone, ... }`
* `SDDPOptions::ElasticFilter { mode, infeasibility_penalty, fcut_max, ... }`
* `SDDPOptions::LowMemory { mode, codec, dump_dir, verify_matind, ... }`
* `SDDPOptions::Logging { trace_log, log_directory, lp_debug, ... }`
* `SDDPOptions::Recovery { mode, ... }`

JSON parsing migrates via nested objects (with `--set
sddp_options.aperture.count=10` syntax).

**Why:** Right now finding the option you want requires grepping the
whole struct.  Grouping documents the conceptual boundaries and
surfaces orthogonality.

**Risk:** medium.  JSON-schema-breaking.  Need a back-compat parser
layer.

### B4. Property-based testing harness — ~2 weeks

**What:** Add a `test_property/` directory with property tests over
LP transformations:

* "off-mode resolve == compress-mode resolve at every iter"
  (parameterised over 10 randomly-generated SDDP fixtures).
* "matind sortedness invariant holds across every flatten() output".
* "after release_backend → reconstruct_backend → resolve, the final
  state matches a fresh solve".
* "aperture-on with single matching aperture == aperture-off" (the
  user's clever equivalence test from earlier today, generalized).

Use rapidcheck or doctest's bench facility.

**Why:** Trajectory equivalence is currently tested empirically on a
single juan/iplp run.  Property tests scale this to many fixtures and
catch corner cases (e.g., reservoirs with zero capacity, single-stage
cases, etc.).

**Risk:** low.  Properties are slow but don't gate CI.

## C. Strategic / architectural (1+ month each)

### C1. Plugin-folder-drop registration — ~1 month

**What:** Each backend currently requires modifying
`plugins/CMakeLists.txt`, the `solver_registry.cpp` priority list,
and a new C++ pair.  Convert to: drop a `plugins/<name>/` folder with
a `manifest.json` (priority, name, capabilities) — CMake auto-discovers
via `FILE(GLOB plugins/*/CMakeLists.txt)`.

**Why:** Adding a new solver (we discussed Gurobi/MindOpt earlier) is
a 3-file change today.  Folder-drop makes it a one-folder change.

**Risk:** medium.  CMake glob is a known anti-pattern (no incremental
rebuild on file add).  Mitigate with a manifest list at top-level.

### C2. Element CRTP registry — ~1.5 months

**What:** Adding a new `XxxLP` class today touches: `Xxx.hpp`
(struct), `XxxLP.hpp/cpp` (LP class), `xxx_writer.py` (plp2gtopt),
`validate_planning.cpp`, `system.hpp` (`xxx_array`), JSON binding,
integration test.  Convert to CRTP-based registration: each `XxxLP`
declares its capabilities (state vars, update_lp, validation rules)
via traits; the system walks the trait list to dispatch.

**Why:** I touched ~8 files for each of the validate_planning +
plp2gtopt segment-feasibility additions.  CRTP would reduce to 1.

**Risk:** high.  CRTP across this many element types is non-trivial
and may slow down compile time.

### C3. Strong typing for state-variable interactions — ~3 weeks

**What:** State-var interactions (`col_sol_physical()`,
`reduced_cost_physical(scale_obj)`, `var_scale()`) are duck-typed
today.  Add a `StateVariableView` interface that captures the
contract; refactor the 4–5 call sites in `benders_cut.cpp` and
`sddp_method_alpha.cpp` to use it.

**Why:** The off↔compress investigation had to trace these
interactions across 4 files.  Strong typing would let me ask the
compiler "where do state-var values flow?" instead of grep.

**Risk:** medium.  Mostly a rename + interface extraction.

## TL;DR — what I'd do first

If I had 1 week, I'd land **A1 (release_in_place)** + **A5 (CI parity
test)** — they together remove 30% of compress overhead AND prevent
the entire class of bugs that consumed this session.

If I had 1 month, add **B1 (always-re-issue contract)** + **B3
(SDDPOptions grouping)** + **B4 (property tests)**.  Those three
together would have prevented this session's investigation entirely,
and make the next person's debugging 5× faster.

The C-series items are nice-to-have but premature optimization until
A/B reveal that contributor friction (rather than runtime perf or bug
rate) is the actual bottleneck.
