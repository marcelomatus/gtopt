# ComputePool design — replacing the conflated SDDP WorkPool

> **⚠ EXPERT REVIEW VERDICT (2026-06-08): NOT sound as written — do not start the
> Taskflow spike.** The separation-of-concerns *diagnosis* (§3) is right; the
> Taskflow *vehicle* has three P0 design-killers:
> - **P0-1:** Taskflow's work-stealing executor and gtopt's fine-grained global
>   priority are mutually exclusive — you get strict ordering OR work-stealing,
>   not both. An "external ready-queue feeding the executor" collapses Taskflow
>   into a bare thread-runtime *and* breaks the `corun` reentrancy that was the
>   only reason to adopt it.
> - **P0-2:** `corun` + the measured-RSS governor cannot bound peak RSS — a parent
>   holding a multi-GB LP that `corun`-runs *another* heavy task on the same
>   thread holds two LPs' RSS at once. The current `SlotReleaseGuard` releases the
>   slot but **blocks** in `fut.get()` (does NOT run another heavy task) — the
>   crucial difference `corun` erases.
> - **P0-3:** deadlock-freedom fails *with the governor in the loop*: all workers
>   become `corun`-parents, children can't start because RSS is over budget, no
>   child runs → no RSS freed → governor never reopens → livelock. The current
>   design decouples worker count from memory throttling precisely to avoid this.
>
> **Recommended path instead (lower risk, delivers §3's win): fix the EXISTING
> `BasicWorkPool` in place** — (1) add the `gate_bypass` boolean (split the
> `TaskPriority` overload so a task can be ordered-low yet gate-bypassed; this
> alone dissolves the P1-2 sim ⇄ wedge trap), (2) compare the priority key alone
> (no tier shadowing), (3) route BOTH engines (`solve_async` + the coordinator
> path) through ONE pool so priority is honored everywhere (closes P0-1 of the
> *pool* review), (4) keep the proven decoupled-worker + slot-release model (it
> already avoids the reentrancy deadlock WITHOUT `corun`). Also: make laggard-first
> a **tie-break under a spread bound**, not a hard global order (else it
> serializes on the forward data-dependency); and prove `phase_rank` has live
> competitors before adding it (forward phases are data-dependent → likely
> backward-only). Full review in the 2026-06-08 session transcript.

**Status:** design SUPERSEDED by the review verdict above — kept for the
separation-of-concerns analysis (§1-§3), which remains valid.
**Original status:** design (pre-implementation)
**Date:** 2026-06-08
**Branch:** `feat/two-tier-workpool`
**Author:** design captured from the 2026-06-08 review session

> **✅ IMPLEMENTED (2026-06-09) — recommended path landed, full suite green
> (4238/4238).** Uncommitted on `feat/two-tier-workpool`.
>
> - **Step 1 — split the `TaskPriority` overload.** Added a `gate_bypass`
>   boolean to `BasicTaskRequirements` (`work_pool.hpp`); `Task::operator<`
>   now orders by `priority_key` **alone** (the `TaskPriority` tier no longer
>   participates in queue ordering); the CPU gate in `can_dispatch_task` uses
>   `gate_bypass` (not `High`) for the `+5 %` relaxation, `Critical` keeps its
>   95 % + memory relaxations. The SDDP sim task (`sddp_iteration.cpp`) is now
>   `Medium` + `gate_bypass = true` (was `High`) — bypasses the CPU-wedge
>   without reordering ahead of still-training peers, dissolving the P1-2
>   sim ⇄ wedge trap. Safe: every non-SDDP pool uses a single priority per
>   instance, so dropping the tier from ordering only affects the SDDP pool.
> - **Step 2 — make the synchronous coordinator path the default**
>   (`sddp_max_async_spread()` `value_or(1)` → `value_or(0)`). Benchmarking
>   found the lockstep coordinator path faster at every level (warmup −19 %,
>   uninodal −35 %, transport −38 %, identical bounds); the async path pays an
>   after-convergence overshoot and a shared-pool dispatch funnel, and
>   scene-level priority is moot under lockstep. **This supersedes the design's
>   "route both engines through one pool" (§4 step 3):** the data showed the
>   non-pool coordinator path is the faster survivor, so unifying onto the pool
>   would regress speed. Sidesteps pool-review **P0-1** — the priority-buggy
>   async path is now opt-in (`> 0`).
> - **Step 3 — laggard-first `phase_rank` tie-break.** `SDDPTaskKey` is now the
>   4-tuple `(iteration, is_backward, phase_rank, kind)`;
>   `make_aperture_submit_fn` sets `phase_rank = (n_phases-1) - phase` for
>   backward aperture chunks so the laggard scene drains first when two scenes
>   share an iteration's backward sweep at different phases. **Non-negative**
>   (never inverts fwd-before-bwd) and **backward-only in practice** (forward
>   runs all phases as one inline task). The review's "prove competitors first"
>   caveat is satisfied: intra-scene phase competition is impossible (backward
>   is strictly sequential across phases) but cross-scene competition is live
>   in the async path. Closes pool-review **P0-2**.
> - **Engine unification (former step 3) — superseded, not done.** With sync
>   the default and the priority key correct, both engines behave correctly;
>   the data argues against funneling the faster coordinator path through the
>   pool. The two-tier model (`CoordinatorPool` Tier 1 + `SDDPWorkPool` Tier 2)
>   stands as the workable pool; full retirement of the single-tier async
>   engine is left as optional future cleanup.
>
> Tests: `test_work_pool.cpp`, `test_sddp_pool.cpp`,
> `test_planning_method_dispatch.cpp`.

## 1. Problem statement

The current `BasicWorkPool` / `SDDPWorkPool` (`include/gtopt/work_pool.hpp`,
`include/gtopt/sddp_pool.hpp`) collapses **four independent concerns into one
mechanism**, and every attempted fix trades one bug for another. The review on
2026-06-08 found three symptoms that are all the *same* root cause:

| symptom | evidence | root cause |
|---|---|---|
| sim task overrides lower-iteration training ("priority reversed") | `Task::operator<` compares `TaskPriority` *before* the `SDDPTaskKey` tuple (`work_pool.hpp:277-287`); sim submitted `High` (`sddp_iteration.cpp:1551`), training `Medium` (`:1625,:1671`) | `TaskPriority` means **both** ordering rank **and** CPU-gate-bypass class |
| can't fix the above by demoting sim to `Medium` | comment `sddp_iteration.cpp:1531-1547`: `Medium` reintroduces the scene-12 wedge (6-min 0% CPU stall) | same overload — there is no "ordered low **and** gate-bypassed" |
| priority dead on the coordinator path | `CoordinatorPool` runs scene drivers on bare `std::async`, solves `li.resolve()` directly; `make_forward_lp_task_req` has no callers | two parallel orchestration engines, only one consults the key |

The four conflated concerns:

1. **Ordering** — which ready task runs next (the `SDDPTaskKey` tuple).
2. **Admission / CPU-gate** — whether to dispatch under sustained load
   (`max_cpu_threshold`), with `TaskPriority::High`/`Critical` doubling as the
   gate-bypass.
3. **Reentrancy / slot-release-while-blocking** — a parent task releasing its
   worker slot while it blocks on child futures so parents + children sharing
   the pool don't deadlock (`SlotReleaseGuard`).
4. **Memory governance** — the measured-RSS controller that grows/shrinks the
   worker count.

Because these share one `TaskPriority` knob and one task class, they cannot be
tuned independently. "Always another issue shows up" is **structural**.

## 2. Goals / non-goals

**Goals**
- One **governed ComputePool** with the four concerns **separated and
  first-class**.
- Used by the three *managed* clients: **build LP**, **solve LP**,
  **write_out LP**. Only **solve** needs ordering priority.
- Provably **deadlock-free** under the parent-blocks-on-children pattern at the
  worker-count boundary (the failure mode that killed two prior attempts).
- Each migration step independently testable and mergeable.

**Non-goals**
- Governing the *light* pools (alpha-setup, backend-release, gzip) — they keep
  their current ungoverned form.
- Changing SDDP math, cut logic, or convergence.
- A big-bang replacement. The old pool stays until each client is migrated and
  green.

## 2.5 Library base — Taskflow (decided)

Do **not** hand-roll the executor. The deadlock-critical primitive (a worker
that waits on its children must not hold its thread hostage) is natively solved
by work-stealing executors that keep the waiting worker in the steal loop. Use a
battle-tested one and layer gtopt's domain logic on top.

| candidate | reentrancy (parent waits on children) | verdict |
|---|---|---|
| **Taskflow** | `tf::Executor::corun` / `corun_until` keeps the caller worker running other tasks while it waits — recursive/nested parallelism is its core use case. Header-only, MIT, C++17/20, actively maintained. | **chosen** |
| oneTBB | work-stealing services other tasks while waiting, but heavier dep + documented `task_arena` `max_concurrency` deadlock pitfalls | fallback |
| BS::thread_pool | **no** help-while-waiting — a worker blocking on a child future holds its thread → the exact deadlock prior attempts hit | rejected ("naive swap") |

**Split of responsibility:**
- **Taskflow owns** threads + the work-stealing scheduler + `corun` reentrancy
  (the hard, well-tested part). Integrate via CPM (header-only).
- **gtopt owns** two thin layers Taskflow deliberately does *not* provide:
  1. an **admission governor** (CPU-load threshold + measured-RSS budget) gating
     task *start*;
  2. a **priority queue** for ordering — Taskflow's native priority is only 3
     coarse levels (`MIN/NORMAL/MAX`), too coarse for the
     `(iteration, fwd/bwd, kind)` tuple, so we keep our own ready-queue that
     feeds the executor in tuple order.

References: [Taskflow Executor / corun](https://taskflow.github.io/taskflow/classtf_1_1Executor.html),
[Runtime Tasking](https://taskflow.github.io/taskflow/RuntimeTasking.html),
[oneTBB nested-deadlock issue #1316](https://github.com/oneapi-src/oneTBB/issues/1316).

## 3. Separated concerns → ComputePool surface

```
class ComputePool {
  // ── admission (concern 2 + 4) ───────────────────────────────
  //   one governor: CPU budget + measured-RSS budget. Decides
  //   *whether* a ready task may start. NOT coupled to ordering.
  struct Admission { double cpu_factor; double mem_limit_mb; bool gate_bypass; };

  // ── ordering (concern 1) ────────────────────────────────────
  //   an opaque comparable key; compared ALONE (no tier shadowing).
  //   Only the solve client supplies a non-default key.
  template <class Key> ... ;   // Key default = monostate (FIFO)

  // ── reentrancy (concern 3) — FIRST CLASS ────────────────────
  //   submit_blocking(): the calling worker yields its slot to the
  //   governor for the duration of the wait, then re-acquires before
  //   continuing. Replaces SlotReleaseGuard.
  template <class F> future<R> submit(F&&, Admission, Key = {});
  template <class F> R         await_children(span<future<...>>);  // slot-released
};
```

Key separations vs today:

- **`gate_bypass` is a separate boolean on `Admission`, NOT a higher ordering
  tier.** The sim task becomes `{ordering_key = (iter,fwd,lp)}` +
  `{gate_bypass = true}` — correctly ranked by iteration **and** immune to the
  CPU-gate wedge. This single change dissolves the P1-2 ⇄ wedge dilemma.
- **Ordering key is compared alone.** No `TaskPriority`-before-key shadowing, so
  "lower iteration first / forward before backward / lp before non-lp" holds
  globally for solve tasks, not just within a tier.
- **`await_children` is the explicit reentrancy primitive.** A parent solve that
  fans out aperture children calls `await_children`, which hands its slot back to
  the governor while it blocks, so the children can run on that slot. Today this
  is the fragile `SlotReleaseGuard` + `cell_task_headroom` workaround.

## 4. The reentrancy contract (the deadlock-critical spike)

This is the piece a naive `BS::thread_pool` swap cannot express and the reason
prior attempts died.

**Contract:** With a worker cap of `W`, a task may submit `K` child tasks onto
the *same* pool and block on all of them via `await_children` **without
deadlock, for any `K` and any nesting depth**, including when all `W` workers are
simultaneously parents waiting on children.

**Mechanism (Taskflow-backed):** `await_children` is implemented on top of
`tf::Executor::corun_until` — the calling worker stays in the work-stealing loop
and executes other ready tasks until its children complete, instead of blocking
its thread. Taskflow guarantees forward progress here, which is precisely the
property a hand-rolled `SlotReleaseGuard` had to re-derive (and got wrong twice).
The gtopt admission governor is informed of the corun so a re-entered worker is
not double-counted against the CPU/RSS budget while it cooperatively runs other
work.

**Contract test (must exist before any client migrates):**
`test_compute_pool_reentrancy.cpp` — a pool with `W=2`; submit 2 parents that
each submit 3 children and `await_children`; assert all 8 complete and the
observed max concurrent *running* tasks never exceeds `W`, and the run does not
hang under a watchdog timeout.

## 5. Ordering design + contract test

**Governing principle: always run the cell that is furthest *behind* first.**
The entire priority key is just a lexicographic measure of "how far behind a
cell is" — lower iteration is behind, and within a sweep the cell that has made
the least progress is behind (small phase in the forward 0→N sweep, large phase
in the backward N→0 sweep). Scheduling laggards first keeps every scene marching
in lockstep through the sweep, so no scene races ahead to compute work that gets
discarded at a convergence/level boundary — the priority policy and the
overshoot-avoidance goal are the *same* thing.

**Priority is the *primary* alignment mechanism; the spread gate is a backstop.**
Most of today's async pain traces to the priority being wrong (no phase term →
no phase-level alignment; the `High`-tier sim override misorders; the coordinator
path ignores it entirely), so the system compensates with a load-bearing
`max_async_spread` gate, cut-drain logic, and headroom hacks. With a *correct*
laggard-first priority a fast scene's iter-K+1 task always loses to any laggard's
iter-K task, so fast scenes wait by *ordering* alone and the gate's alignment job
mostly disappears. Two residuals keep the gate (thin) and require a separate fix:
(1) **spare workers** — with more free workers than pending laggard tasks at an
instant, a worker still picks up an ahead-task, so keep a small spread bound as a
backstop; (2) **trailing-edge convergence** — the boundary overshoot is a timing
bug (convergence at iter K is known only after *all* scenes finish K, and
submitted tasks can't be recalled), fixed by not dispatching K+1 until K is
finalized (§ review P0-3), independent of priority. Net: correct priority shrinks
the overshoot from "a full discarded iteration" to "a couple of in-flight tasks"
and demotes the gate from mechanism to safety bound.

Solve-LP tasks are scheduled at **per-(scene, phase) granularity** (one task per
cell — the natural granularity of the solve client) and carry the 4-field key:

```
key = (iteration, is_backward, phase_rank, kind)      std::less, lexicographic, NO tier shadow

  iteration   : ascending                         smallest iteration runs first
  is_backward : forward(0) < backward(1)           forward before backward, same iteration
  phase_rank  : forward  → phase_index             ascending  (SMALL phase first)
                backward → (n_phases-1 - phase)    descending (LARGE phase first — the
                                                   backward sweep runs phase N→0)
  kind        : lp(0) < non_lp(1)
```

Rationale for `phase_rank` flipping by direction: the forward sweep propagates
state phase 0→N, so the cell furthest *behind* (smallest phase) should run first;
the backward sweep propagates cuts phase N→0, so the cell furthest behind
(largest phase) should run first. Across scenes this keeps them phase-aligned
within a sweep — the phase-level analogue of the iteration `max_async_spread`.
Equivalently expressible as a single monotonic "stage rank" (forward `p → p`,
`0..N-1`; backward `p → N + (N-1-p)`, `N..2N-1`), matching the literal
`(iteration, fwd/bwd-index, kind)` 3-tuple.

> **⚠ Encoding trap — do NOT use a raw negative sign for the backward index.**
> A naive single field `forward → +phase, backward → −phase` is WRONG: negatives
> sort before positives, so it puts **backward first** (and collides fwd-p0 with
> bwd-p0). Worked example, N=3: values `−2,−1,0,1,2` → `bwd-p2, bwd-p1,
> {fwd-p0≡bwd-p0}, fwd-p1, fwd-p2`. Always encode "descending phase" as a
> **non-negative offset** (`n_phases-1-phase`, or the `N+(N-1-p)` stage-rank
> band), never as `-phase`. The two-field form keeps `is_backward` as a separate
> discriminator precisely so forward strictly precedes backward regardless of the
> phase term.

> This **reverses** the current code's choice (`sddp_pool.hpp:46-53` dropped the
> phase field because the old pool ran all phases inside one per-scene task).
> The ComputePool's per-cell solve granularity makes the phase field live again.

**Contract test:** `test_compute_pool_priority.cpp` — single-worker pool, hold
the worker with a gate task, submit solve tasks in *scrambled* key order across
two iterations, both directions, several phases, lp/non_lp; release the gate;
record execution order; assert it equals the sorted-by-key order. Must cover:
(a) lower iteration before higher; (b) forward before backward in the same
iteration; (c) **forward ascending phase**; (d) **backward descending phase**;
(e) lp before non_lp at an otherwise-equal key. This is the test requested in
the 2026-06-08 session; it doubles as the ComputePool ordering contract.

## 6. Client migration order (each step green before the next)

1. **Spike**: ComputePool core + the two contract tests (§4, §5). No client yet.
2. **build LP**: migrate `PlanningLP` initial build pool (no ordering, governed
   admission). Lowest risk — no reentrancy.
3. **write_out LP**: migrate the per-cell write_out fan-out (no ordering,
   governed; bounded reentrancy if scene→phase nesting is used).
4. **solve LP**: migrate the SDDP forward/backward/aperture path — the only
   client needing ordering **and** reentrancy. Retire `SDDPWorkPool`,
   `SlotReleaseGuard`, `cell_task_headroom`, and the `High`-tier sim hack.
5. **Unify the two engines**: route the `solve_async` and the coordinator/sync
   paths through the one ComputePool so priority is honored on *both* (closes
   review finding P0-1). `CoordinatorPool` becomes a thin driver over it or is
   deleted.

The light pools (alpha-setup, backend-release, gzip) are untouched.

## 7. Risks

- **Reentrancy correctness** is the whole ballgame; §4's test must be solid and
  run under a hard watchdog in CI before step 2.
- **Governor fairness**: re-acquiring parents must not starve fresh leaves
  (use FIFO re-acquire or a small reserved re-acquire quota).
- **Behavior parity**: solve-client migration must reproduce today's bounds
  bit-for-bit on juan (the existing `low_memory=off` reference oracle); diff the
  convergence table per level.

## 8. Open questions

- ~~Engine: hand-rolled vs library~~ — **resolved (§2.5): Taskflow.** Its
  `corun` solves the reentrancy that hand-rolling and `BS::thread_pool` cannot.
- **Priority granularity:** keep a gtopt ready-queue (full `(iteration,fwd/bwd,
  kind)` order) feeding the executor, vs collapse onto Taskflow's 3-level native
  priority (likely too coarse — iterations exceed 3). Leaning gtopt ready-queue.
- **Governor ↔ corun accounting:** a worker that `corun`s while waiting is doing
  useful work but shouldn't count as a fresh admission. Define the exact
  semaphore accounting so the CPU/RSS budget stays honest under re-entry.
- Whether `gate_bypass` should be a per-task boolean or a small enum of
  admission classes (e.g. `normal` / `bypass` / `critical`).
- Taskflow integration: pin a version via CPM; confirm C++26 build cleanliness
  (it targets C++17/20 — check for `-Wpedantic -Werror` warnings under Clang 21 /
  g++-15).
