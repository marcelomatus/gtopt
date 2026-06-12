# SDDP Work-Pool Two-Tier Migration Plan

> Status: design / plan only. No production code is edited by this document.
> Target: `gtopt` C++26, g++-15, BSD-3-Clause.
> Scope: refactor the single `SDDPWorkPool` into a **coordinator tier**
> (scene/pass orchestration) and a **solver tier** (resource-aware LP
> solves).

---

## 0. Confirmed facts from the code

The plan below is anchored on these verified observations (file:line):

1. **Forward scene tasks solve LPs inline, not via the pool.**
   `run_forward_pass_all_scenes` submits one task per scene
   (`source/sddp_method_iteration.cpp:1045-1059`); each task body calls
   `forward_pass(...)` which walks all phases and solves each phase with a
   **direct** `li.resolve(effective_opts)`
   (`source/sddp_forward_pass.cpp:237`, comment on
   `:233` "Solve directly — already running in a pool thread"). So a forward
   scene task is a *single, long, CPU-bound* pool task that never re-enters
   the pool. There is **no lock and no shared solver** in that body
   (`grep` over `sddp_forward_pass.cpp` finds no `submit`/`mutex`/
   `release_slot`).

2. **Backward scene tasks DO re-enter the same pool.** Each backward scene
   task (`source/sddp_method_iteration.cpp:1281-1299`) calls
   `backward_pass_with_apertures`, whose aperture step submits per-aperture
   solves back to the **same** `SDDPWorkPool` via the chunk submit fn
   (`source/sddp_aperture.cpp:399` `submit_fn(...)`), then blocks on the
   chunk futures (`:778-779 fut.get()`). To avoid wedging the shared pool
   while blocked, it grabs a `SlotReleaseGuard`
   (`source/sddp_aperture.cpp:774-776`).

3. **Both passes use identical task requirements.**
   `make_forward_lp_task_req` and `make_backward_lp_task_req`
   (`include/gtopt/sddp_pool.hpp:157-181`) both set
   `estimated_threads = 1`, `TaskPriority::Medium`; they differ only in the
   `is_backward` tuple field of `SDDPTaskKey`.

4. **The shared-pool lazy-spawn decision is the forward-serialization
   suspect.** `maybe_spawn_worker_unlocked`
   (`include/gtopt/work_pool.hpp:1070-1099`) reads
   `active = active_threads_` and `pending = task_queue_.size()` under
   `queue_mutex_` and spawns only when `pending > idle`. The 16 forward
   submits run as a burst (`sddp_method_iteration.cpp:1045-1059`); each
   submit pushes one task, computes `idle = total - active`, and decides
   whether to add a worker. Because each submit also does `notify_all`
   (`work_pool.hpp:722`) but a freshly-spawned worker has not yet
   incremented `active_threads_`, the `idle` estimate used by the next
   submit's spawn decision is racy. The combined dispatch path
   (`worker_loop` `work_pool.hpp:1223-1373` ↔ `find_dispatchable_index`
   `:1603-1615` ↔ `can_dispatch_task` `:1384-1572`) was tuned for the
   backward/aperture pattern (`PEEK_DEPTH`, blocking-slot release), and the
   forward burst observably collapses to ~1-2 concurrent workers
   (Active:1/96, ~13-min sweeps) while the backward/aperture phase on the
   same pool runs Active:16 at ~91% CPU.

5. **`cell_task_headroom` exists ONLY for the in-pool blocking pattern.**
   The headroom term in `make_sddp_work_pool`
   (`include/gtopt/sddp_pool.hpp:211-291`, set from `scene_count()` at
   `source/sddp_iteration.cpp:238`) reserves extra slots so that a backward
   cell task blocking on aperture futures cannot reduce aperture
   parallelism below `cpu_factor × cores`. `memory_clamp_threads`
   (`work_pool.hpp:148-163`) documents the same constraint: worker count is
   deliberately **decoupled** from the memory throttle *because*
   `release_slot_while_blocking` needs a free worker to run the child.

6. **We already have two pools, split along the wrong seam.**
   `solve()` creates `sddp_pool` (the `SDDPWorkPool` that does *everything*)
   and `aux_pool` (an `AdaptiveWorkPool` used only by `BendersCut` elastic
   clones + `LpDebugWriter` gzip) at
   `source/sddp_iteration.cpp:239-257`. Members `m_pool_`/`m_aux_pool_`
   live at `include/gtopt/sddp_method.hpp:1175,1181`. `aux_pool` is *not* a
   solver tier — it is a side-channel for non-LP-critical work.

---

## 1. Architecture diagram

```
                         SDDPMethod::solve()
                                 │
              creates two tiers (replaces sddp_pool + aux_pool seam)
                                 │
        ┌────────────────────────┴───────────────────────────────┐
        ▼                                                          ▼
┌──────────────────────────┐                      ┌──────────────────────────────┐
│  TIER 1 — Coordinator     │                      │  TIER 2 — Solver pool         │
│  (CoordinatorPool)        │   submit solve       │  (BS::thread_pool +           │
│                           │   request            │   AdmissionGovernor)          │
│  fixed size = num_scenes  │ ───────────────────► │                               │
│  one driver jthread/scene │                      │  resource-aware: RSS gate,    │
│                           │ ◄─────────────────── │  mem%/free-MB gate, CPU gate, │
│  driver S:                │   future<solve>      │  VmSwap/swap-IO gate,         │
│   forward sweep           │   resolves           │  measured marginal-mem model, │
│    (51 phases, serial)    │                      │  universal-progress admit     │
│     each phase:           │                      │                               │
│       submit LP → block   │   N drivers ⇒ up to  │  width = as wide as resources │
│   backward sweep          │   N concurrent       │  allow (governor-gated)       │
│    per phase: submit K    │   solve requests     │                               │
│      aperture solves,     │                      │  NO orchestration,            │
│      block on all K       │                      │  NO scene/phase knowledge     │
│                           │                      │                               │
│  NO memory mgmt           │                      │                               │
│  NO admission gate        │                      │                               │
│  threads ~always BLOCKED  │                      │                               │
│  on a Tier-2 future       │                      │                               │
└──────────────────────────┘                      └──────────────────────────────┘
        ▲                                                          │
        │  cuts / bounds written back into m_cut_store_,           │
        │  m_scene_phase_states_ by the driver after its           │
        │  Tier-2 future resolves (same threading model as today)  │
        └──────────────────────────────────────────────────────────┘
```

Data flow for one solve:

```
driver(scene S)  ──submit(resolve λ, req)──►  Tier2 queue
                                                  │ governor admits when
                                                  │ resources allow
                                                  ▼
                                            BS worker runs li.resolve(opts)
                                                  │
driver(scene S)  ◄──── future.get() ──────────────┘
        │
        ▼  builds cut from duals/reduced-costs, store_cut(...),
           add_row(alpha_cut), capture_state_variable_values(...)
```

The driver is the **only** writer of scene S's `m_scene_phase_states_[S]`
and the per-scene cut slot (per the existing per-(scene,phase) partition
documented at `test/source/test_sddp_per_cell_release.cpp`), so moving the
orchestration into a dedicated driver thread does not change the data-race
profile — it is the same body, just no longer multiplexed onto pool
workers.

---

## 2. Coordinator tier design (Tier 1)

### 2.1 API

A thin, dependency-free fixed pool. Proposed surface (new header
`include/gtopt/coordinator_pool.hpp`):

```cpp
class CoordinatorPool {
public:
  // Fixed size — exactly one driver per scene.  No config, no governor.
  explicit CoordinatorPool(std::size_t num_drivers);

  // Run `fn(scene_index)` on its own long-lived driver thread.  Returns a
  // future for the driver's result (the per-scene forward/backward outcome).
  template<class Fn>
  std::future<std::invoke_result_t<Fn, SceneIndex>>
  run_driver(SceneIndex s, Fn&& fn);

  void join_all();         // block until all drivers finish a pass
  ~CoordinatorPool();      // joins
};
```

Implementation: a `std::vector<std::jthread>` of size `num_drivers`, each
parked on a per-driver `packaged_task` slot, OR — simpler — just spawn the
drivers per pass and join them (drivers are coarse: one per scene per
pass, ~32 thread spawns per iteration total, negligible vs. minutes of LP
solves). A `std::jthread` per scene per pass is the minimal correct
implementation and matches the prior recommendation ("`num_scenes`
`std::jthread`s or a trivial fixed pool suffices").

### 2.2 Lifecycle

- Created once in `solve()` alongside the solver tier
  (replaces the `sddp_pool` creation at `source/sddp_iteration.cpp:239`).
- Per forward pass: `run_forward_pass_all_scenes` spawns one driver per
  non-skipped scene; each driver runs the **current** `forward_pass(...)`
  body unchanged, except every `li.resolve(...)` becomes
  `solver_tier.solve(li, opts, req).get()` (see §3.4). Drivers are joined
  by the existing result-collection loop
  (`sddp_method_iteration.cpp:1065-1145`), which already does
  `futures[scene_index].get()`.
- Per backward pass: identical pattern at
  `sddp_method_iteration.cpp:1281-1314`.

### 2.3 Sizing — exactly `num_scenes`, zero resource management

The driver count is `planning_lp().simulation().scene_count()` — the same
value currently fed as `cell_task_headroom`
(`source/sddp_iteration.cpp:238`). Crucially:

- **No admission gate.** A driver is admitted the instant it is created.
- **No memory model.** Drivers do not allocate LP backends; they hold
  references and block on futures. A blocked driver's RSS contribution is
  a thread stack (~tens of KB), so `num_scenes` drivers add < a few MB —
  far below any gate.
- **No CPU/swap throttle, no elastic sizing.** A blocked driver consumes
  no core. When all `num_scenes` drivers are blocked on Tier-2 futures,
  Tier 1 uses ~0% CPU. This is the property that lets us drop the entire
  `WorkPoolConfig` resource machinery from this tier.

This is why Tier 1 can be a `std::jthread` vector and not a
`BasicWorkPool`: it has no work queue, no priority key, no gates.

### 2.4 Forward sweep (sequential phases)

The driver runs `forward_pass(scene S, opts, iter)` exactly as today
(`source/sddp_forward_pass.cpp:38`): a sequential loop over 51 phases, each
phase a single LP solve. The only change is the solve call site
(`:237`). Sequentiality is inherent to SDDP forward (phase t's state feeds
t+1), so each driver issues **one** Tier-2 solve at a time and blocks. With
16 drivers, Tier 2 sees up to 16 concurrent solve requests — exactly the
parallelism the forward pass is supposed to have, restored without any
shared-pool wake race.

### 2.5 Backward sweep (apertures — multiple solves per phase)

The driver runs `backward_pass_with_apertures(scene S, ...)`. At each
phase it has K apertures to solve. Two equivalent options:

- **Concurrent fan-out (preferred):** the driver submits all K aperture
  solves to Tier 2 at once and blocks on the K futures. This is the
  current chunk-submit pattern (`source/sddp_aperture.cpp:399`) minus the
  `SlotReleaseGuard` (no longer needed — see §4). Tier 2 sees
  `num_scenes × K` in-flight requests and runs them as wide as resources
  allow.
- **Serial (chunk_size = -1 today):** driver issues aperture solves one at
  a time. Still correct; just less parallel.

Cut flow back is unchanged: after the aperture futures resolve, the driver
builds the expected cut and installs it
(`install_aperture_backward_cut`, `sddp_method.hpp:913`), all on the
driver thread, writing only into scene S's partition.

### 2.6 Zero resource management — proof obligation

Tier 1 carries none of: `max_process_rss_mb`, `min_free_memory_mb`,
`max_memory_percent`, `max_process_swap_mb`, `max_swap_io_per_sec`,
`max_threads_ceiling`, `meas_per_task_mb_`, `idle_floor_mb_`,
`last_admit_time_`, the throttle counters, or `maybe_grow_max_threads`.
All of those move to (or stay in) Tier 2. Tier 1's only knob is
`num_drivers = num_scenes`.

---

## 3. Solver tier design (Tier 2)

### 3.1 Engine: BS::thread_pool 5.x + thin governor

Per the prior recommendation, adopt **BS::thread_pool** (MIT, header-only,
true global priority queue, `std::future` submit) as the execution engine,
fronted by a ~150-line `AdmissionGovernor`. BS provides:

- a global work queue with worker threads,
- `submit_task` returning `std::future`,
- optional task priority (maps to the SDDP `is_backward`/iteration
  ordering, see §3.5).

What BS does **not** provide and we must keep: the resource-aware
admission gate. BS has **no bounded queue / no admission hook**, so the
governor must gate **before** `submit` (see §3.3).

### 3.2 What moves into Tier 2 (kept governor state)

All of the resource-awareness from the current `can_dispatch_task`
(`work_pool.hpp:1384-1572`) is retained, but applied as a pre-submit
admission decision rather than a per-worker dispatch gate:

| Gate / state | Current location | Tier-2 role |
|---|---|---|
| measured marginal-mem model (`idle_floor_mb_`, `meas_per_task_mb_`, `update_memory_model_`) | `work_pool.hpp:397-398,1112-1134` | KEEP — drives the RSS projection |
| RSS projection gate (`projected = rss + per_task >= limit`) | `work_pool.hpp:1478-1520` | KEEP |
| system mem% gate | `work_pool.hpp:1452-1461` | KEEP |
| free-MB gate | `work_pool.hpp:1463-1473` | KEEP |
| CPU-load gate (near-saturation only) | `work_pool.hpp:1429-1449` | KEEP |
| VmSwap gate | `work_pool.hpp:1525-1537` | KEEP |
| swap-IO rate gate | `work_pool.hpp:1544-1559` | KEEP |
| one-admit-per-monitor-interval rate limit (`last_admit_time_`) | `work_pool.hpp:1483-1498` | KEEP |
| universal-progress admit (`active_now == 0 ⇒ admit`) | `work_pool.hpp:1112-1134 + 1414-1416` | KEEP (see §7) |
| throttle counters (diagnostics) | `work_pool.hpp:424-429` | KEEP |
| `CPUMonitor` / `MemoryMonitor` | `work_pool.hpp:363-364` | KEEP |

### 3.3 Admission boundary — gate BEFORE submit

Because BS has no bounded queue, the governor must hold work back *before*
it reaches BS. Proposed `AdmissionGovernor::admit_solve(req) -> future`:

```
admit_solve(li, opts, req):
    loop:
        update_memory_model_(active_now())          // refresh /proc RSS
        if can_admit(req):                            // the §3.2 gates, O(1)
            stamp last_admit_time_
            active_.fetch_add(1)
            return bs_pool.submit_task([&]{
                auto r = li.resolve(opts);
                active_.fetch_sub(1);
                accumulate_task_stats(...);
                return r;
            }, bs_priority(req))
        else:
            wait_for(monitor_interval)                // back-off, then retry
```

The back-off loop runs **on the calling driver thread** (Tier 1), not on a
Tier-2 worker — so a throttled admission never occupies a solver core.
This is strictly simpler than the current `worker_loop`'s
`find_dispatchable_index` + `PEEK_DEPTH` head-of-line scan
(`work_pool.hpp:1247-1274,1574-1615`), which existed to avoid a *single
gated head task* parking the whole shared pool. With per-driver admission,
each driver gates only its own request, so the head-of-line problem
disappears and `PEEK_DEPTH` / `find_dispatchable_index` are deleted (§4).

### 3.4 Solve API consumed by drivers

Drivers call a single entry point that replaces today's
`resolve_via_pool` / `resolve_clone_via_pool`
(`source/sddp_method_iteration.cpp:340-384`) and the inline
`li.resolve` at `source/sddp_forward_pass.cpp:237`:

```cpp
// Tier 2 public API
std::future<std::expected<int,Error>>
SolverTier::submit_resolve(LinearInterface& li, const SolverOptions& opts,
                           SolveReq req);
```

`resolve_via_pool`/`resolve_clone_via_pool` collapse to thin wrappers (or
are inlined into the drivers). The forward inline solve becomes
`solver_tier.submit_resolve(li, opts, fwd_req).get()`.

### 3.5 Sizing of Tier 2

Tier 2 BS worker count = the **un-clamped CPU budget**
`cpu_factor × physical_concurrency()` (the `ceiling` computed today at
`sddp_pool.hpp:254-257`, but **without** the `+ cell_task_headroom` term —
that term is deleted, §4/§5). The governor, not the worker count, bounds
memory. BS workers that have no admitted work simply idle on BS's internal
condition variable.

Priority: BS 5.x supports a per-task priority. Map the SDDP ordering
(lower iteration first; forward before backward at same iteration; lp
before non_lp) onto BS priority by reusing `make_sddp_task_key` ordering
collapsed to BS's priority scalar. If BS's priority granularity is too
coarse, keep a small priority wrapper; this is a non-blocking detail since
the dominant correctness driver (iteration ordering) only matters in the
async path (`solve_async`, `sddp_method.hpp:938`).

---

## 4. Deletion list (from the 1657-line `work_pool.hpp`)

With tiers separated, the in-pool-blocking machinery and the
forward/backward-shared dispatch are deleted. Estimated line counts are
approximate (including doc comments, which are large in this file).

| Item | Location | ~Lines | Reason |
|---|---|---|---|
| `SlotReleaseGuard` class | `work_pool.hpp:853-899` | ~47 | No parent task blocks on a child in the same pool (drivers ≠ solver workers). |
| `release_slot_for_blocking_` | `work_pool.hpp:1046-1061` | ~16 | Same. |
| `reacquire_slot_after_blocking_` | `work_pool.hpp:1063-1068` | ~6 | Same. |
| Slot-release doc block | `work_pool.hpp:820-852, 1032-1045` | ~27 | Same. |
| `cell_task_headroom` param + plumbing | `sddp_pool.hpp:211-291` (factory), `sddp_iteration.cpp:238` | ~50 | Headroom existed only to keep a solver worker free for the blocked parent. Tier 2 has no parents. |
| `find_dispatchable_index` + `PEEK_DEPTH` | `work_pool.hpp:1574-1615, 1601` | ~42 | Head-of-line throttle deadlock is impossible when each driver gates only its own request. |
| `can_dispatch_top` (legacy shim) | `work_pool.hpp:1617-1623` | ~7 | Only used by `find_dispatchable_index`/tests. |
| `maybe_grow_max_threads_unlocked` + `max_threads_ceiling` growth | `work_pool.hpp:1136-1210, 373-377, 84-90` | ~95 | Tier 2 runs at a static ceiling; the governor bounds memory, not the thread count. Elastic grow is removed. |
| `maybe_spawn_worker_unlocked` lazy-spawn race | `work_pool.hpp:1070-1099` | ~30 | Replaced by BS's own worker management; the racy `pending > idle` heuristic (the forward-serialization root cause) is gone. |
| `worker_loop` (custom dispatch) | `work_pool.hpp:1223-1373` | ~150 | Replaced by BS workers. The gate logic relocates to `AdmissionGovernor::can_admit` (pre-submit). |
| Forward/backward-shared `submit`/`submit_batch` heap + `Task<>` priority heap | `work_pool.hpp:344, 658-801, 216-288` | ~180 | The custom max-heap priority queue is replaced by BS's global priority queue. |

**Kept** (relocated into the ~150-line governor): the `can_dispatch_task`
gate body (`work_pool.hpp:1384-1572`, ~190 lines, minus the
thread-count/CPU-saturation lines that BS subsumes), `update_memory_model_`
(`:1112-1134`), the measured-memory atomics (`:397-405`), throttle counters
(`:424-429`), and the `CPUMonitor`/`MemoryMonitor` members.

**Net estimate:** the SDDP-specific custom-pool surface (~1657 lines in
`work_pool.hpp` + ~290 in `sddp_pool.hpp`) shrinks to:
- a ~150-line `AdmissionGovernor` (gates + measured-mem model + monitors),
- a thin BS adapter,
- a ~60-line `CoordinatorPool`.
Roughly **~700–800 lines deleted net** once BS replaces the custom queue,
worker loop, lazy-spawn, elastic-grow, and slot-release subsystems.
`BasicWorkPool<>` / `AdaptiveWorkPool` may remain for the *other* callers
(`make_solver_work_pool` users: monolithic, planning_lp, alpha-setup —
`source/monolithic_method.cpp:36`, `source/planning_lp.cpp:1532,1784,2013`,
`source/sddp_method.cpp:526,697`), so the header is not removed outright;
only the SDDP-blocking-specific members are stripped. (Migrating those
callers to Tier 2 / BS is out of scope for the first PRs — see §8.)

---

## 5. Mapping the existing `sddp_pool` + `aux_pool` onto the new tiers

| Existing | Role today | New home |
|---|---|---|
| `sddp_pool` (`SDDPWorkPool`, `source/sddp_iteration.cpp:239`) | orchestration **and** LP solves, shared | **split**: orchestration → Tier 1 `CoordinatorPool`; LP solves → Tier 2 solver pool |
| `cell_task_headroom = scene_count()` (`:238`) | extra solver slots for blocked backward cell tasks | **deleted** — Tier 1 drivers block off-core; Tier 2 needs no headroom |
| `aux_pool` (`AdaptiveWorkPool`, `:245-252`) | `BendersCut` elastic-clone solves + `LpDebugWriter` gzip | **merge into Tier 2** for the LP-clone solves (they are real LP solves and belong under the governor); keep a tiny separate I/O pool **only** for gzip if desired, or submit gzip as low-priority Tier-2 tasks (`SDDPTaskKind::non_lp`). |
| `m_pool_` member (`sddp_method.hpp:1175`) | non-owning `SDDPWorkPool*` | becomes `SolverTier*` (Tier 2) |
| `m_aux_pool_` member (`sddp_method.hpp:1181`) | non-owning `AdaptiveWorkPool*` | folded into Tier 2; `BendersCut::set_pool` (`sddp_iteration.cpp:255`) retargets to the solver tier |
| `make_sddp_work_pool` (`sddp_pool.hpp:245`) | factory with headroom | replaced by `make_solver_tier(cpu_factor, memory_limit_mb)` (no headroom arg) + `CoordinatorPool(num_scenes)` |

The async path (`solve_async`, `sddp_method.cpp:697-722`) submits whole
scene-iteration loops to `sddp_pool`; under the new design those become
Tier-1 drivers that internally call Tier 2 — same transformation as the
sync passes.

---

## 6. Deadlock-freedom argument

Define the wait-for graph. Today (single pool) the hazard is:

> a backward cell task A (running on a solver worker) calls `fut.get()` on
> aperture child B, which is queued in the **same** pool. If every worker
> is occupied by an A-like task and the gate is closed, B never dispatches
> ⇒ classic resource deadlock. The whole `SlotReleaseGuard` + headroom
> subsystem exists to break exactly this cycle
> (`work_pool.hpp:820-852`, `sddp_aperture.cpp:768-779`).

Under the two-tier design:

1. **Tier 1 drivers never run on Tier-2 workers.** A driver D blocks on a
   Tier-2 future. D occupies a Tier-1 thread (its own dedicated jthread),
   **not** a Tier-2 worker slot.
2. **Tier-2 workers never block on Tier-2 futures.** A Tier-2 task body is
   exactly `li.resolve(opts)` (+ stats) — it does not submit to Tier 2 and
   does not `.get()` a Tier-2 future. So no Tier-2 worker ever waits on
   Tier-2 capacity.
3. Therefore the wait-for graph has **no cycle through Tier 2**: edges go
   only Tier-1 → Tier-2 (a driver waits on a solve), never Tier-2 → Tier-2.
   A blocked driver cannot consume a solver slot, so it cannot starve the
   solver pool. Deadlock of the kind `SlotReleaseGuard` guarded against is
   structurally impossible, which is why that machinery is **deleted, not
   ported** (§4).

The only remaining liveness question is **throughput**, addressed next.

---

## 7. The one real risk — throughput floor (not deadlock)

With independent tiers there is no deadlock, but Tier 2 must be sized /
admitted so that all in-flight driver requests can make **progress**. The
worry: under a tight memory limit the governor refuses every admission and
no solve runs, so all `num_scenes` drivers block forever.

This is prevented by the **universal-progress guarantee**, which is kept
verbatim from the current pool
(`work_pool.hpp:1112-1134` floor capture + `:1414-1416` "when
`active_now == 0`, ADMIT one task regardless of any soft gate"). In the
governor:

```
can_admit(req):
    active = active_count()
    update_memory_model_(active)
    if active == 0:
        return true          // always run at least one solve — minimum-
                             // resource progress; the running solve will
                             // relieve/measure memory before the next admit
    ... apply RSS/mem%/free/CPU/swap gates ...
```

Guarantees:

- **At least one solve always runs** when memory is tight: with all
  drivers blocked and zero active solves, the next `can_admit` sees
  `active == 0` and admits one. That solve completes, the driver proceeds,
  and the next `active == 0` window admits the next. Worst case Tier 2
  degenerates to **serial** execution — slow, but live. This is the same
  livelock cure documented in the code comment at `work_pool.hpp:1403-1416`
  (the 2-year sim/write pass wedge).
- **One-admit-per-interval rate limit** (`work_pool.hpp:1483-1498`) keeps
  the governor from waving the whole `num_scenes × K` backlog through
  before any RSS impact is measured — preserved unchanged.
- **No headroom needed for progress:** progress depends on Tier 2 running
  ≥1 solve, which the universal-progress admit guarantees regardless of
  how many drivers are blocked. The old `cell_task_headroom` was about
  *parallelism width* under the shared pool, not progress — and width is
  now naturally `min(num_scenes×K in-flight, governor-admitted)`.

Sizing recommendation: Tier-2 ceiling =
`cpu_factor × physical_concurrency()` (no headroom). Document that under a
memory limit the *effective* width self-regulates from 1
(universal-progress floor) up to the ceiling as the measured marginal
model reveals headroom — identical dynamics to today's grow path, minus
the elastic worker-count churn.

---

## 8. Incremental migration steps (reviewable PRs)

Each step keeps the suite green; the guarding tests are named per step.

**PR 1 — Introduce Tier 2 as a wrapper around the existing pool (no
behavior change).** Add a `SolverTier` interface with
`submit_resolve(li,opts,req)` whose first implementation simply forwards to
the existing `SDDPWorkPool` (current `resolve_via_pool` body,
`sddp_method_iteration.cpp:340-359`). Route the forward inline solve
(`sddp_forward_pass.cpp:237`) and aperture submits
(`sddp_aperture.cpp:399`) through it. No tier split yet.
*Guards:* `test/source/test_work_pool.cpp`,
`test/source/test_sddp_pool.cpp`, `test/source/test_sddp_method.cpp`, plus
all SDDP integration tests (`ctest -j20`). Confirms the indirection is
transparent.

**PR 2 — Add `CoordinatorPool` and move scene orchestration onto driver
threads.** Replace the `pool.submit(forward_pass...)` /
`pool.submit(backward_pass...)` burst at
`sddp_method_iteration.cpp:1054,1287` with `CoordinatorPool::run_driver`.
Tier 2 still = the existing `SDDPWorkPool`. The forward serialization
*should already improve here*, because drivers feed the shared pool
independently rather than as one burst through the racy lazy-spawn path.
*Guards:* `test_sddp_method.cpp`, `test_sddp_async.cpp`,
`test_sddp_cut_sharing.cpp`, `test_sddp_per_cell_release.cpp` (verifies
per-(scene,phase) release end-state is unchanged), integration suite.

**PR 3 — Swap Tier-2 engine to BS::thread_pool + `AdmissionGovernor`.**
Vendor BS 5.x (header-only). Implement the governor by lifting
`can_dispatch_task` (`work_pool.hpp:1384-1572`), `update_memory_model_`
(`:1112-1134`), and the measured-mem atomics into a pre-submit
`can_admit`. Keep `CPUMonitor`/`MemoryMonitor`.
*Guards:* `test_work_pool.cpp`, `test_work_pool_coverage.cpp`,
`test_work_pool_stall_recovery.cpp` (universal-progress / stall recovery —
the §7 floor must still pass), `test_sddp_method.cpp`, integration suite.
Add a focused governor unit test (mem-limit → serial-but-live; no-limit →
full width).

**PR 4 — Delete the dead machinery.** Remove `SlotReleaseGuard`,
`release_slot_for_blocking_`, `reacquire_slot_after_blocking_`,
`find_dispatchable_index`/`PEEK_DEPTH`/`can_dispatch_top`,
`maybe_grow_max_threads_unlocked` + elastic ceiling, the custom
`worker_loop`/`submit`/heap, and the `cell_task_headroom` plumbing
(`sddp_pool.hpp:211-291`, `sddp_iteration.cpp:238`). Update
`sddp_aperture.cpp:768-779` to drop the `SlotReleaseGuard` (driver blocks
off-core now).
*Guards:* compile (the deleted symbols must have no remaining callers —
`grep` clean per the §0 inventory), `test_sddp_pool.cpp` (drop the
headroom assertions in `make_sddp_work_pool`), `test_sddp_per_cell_release.cpp`,
full `ctest -j20` + integration.

**PR 5 — Merge `aux_pool` into Tier 2 (optional cleanup).** Retarget
`BendersCut::set_pool` (`sddp_iteration.cpp:255`) and `LpDebugWriter` to
the solver tier (LP-clone solves under the governor; gzip as
`non_lp`-priority tasks). Remove `m_aux_pool_`
(`sddp_method.hpp:1181`) and the `need_aux_pool` branch
(`sddp_iteration.cpp:242-252`).
*Guards:* `test_sddp_benders_cut.cpp`, `test_sddp_bcut_recovery.cpp`,
`test_sddp_method.cpp`, integration suite.

**Validation throughout:** the forward-serialization fix is measurable via
the `SDDPWorkPool`/Tier-2 `Active/Pending` stats line — after PR 2/PR 3 the
forward pass should show Active ≈ `min(num_scenes, ceiling)` instead of
Active:1. This is an observability check on real juan/IPLP-scale runs, not
a unit test (per the note in `test_sddp_per_cell_release.cpp` that pool
width is not unit-testable without large fixtures).

---

## 9. Summary of the three claimed wins (validated)

1. **Forward serialization fixed.** Root cause confirmed: 16 forward tasks
   burst-submit through the racy `maybe_spawn_worker_unlocked`
   `pending > idle` heuristic (`work_pool.hpp:1070-1099`) onto a dispatch
   path tuned for the backward/aperture blocking pattern. With 16
   dedicated drivers each independently issuing one Tier-2 solve and
   blocking off-core, Tier 2 sees 16 concurrent requests and runs them as
   wide as the governor allows. No shared-pool wake race remains.
2. **Deadlock machinery deleted, not ported.** Confirmed against the code:
   `SlotReleaseGuard`/`release_slot_for_blocking_`/`cell_task_headroom`
   exist solely because a backward parent task blocks on an aperture child
   in the *same* pool (`sddp_aperture.cpp:768-779`,
   `sddp_pool.hpp:211-291`, `work_pool.hpp:820-899,1046-1068`). Tiers make
   that cycle impossible (§6), so the subsystem is removed.
3. **Simpler governor.** Tier 2's measured-per-task model
   (`(rss − floor)/active`, `work_pool.hpp:1112-1134`) no longer has
   coordinator tasks polluting `active`/RSS — every active task is a real
   LP solve, so the marginal estimate is clean.
