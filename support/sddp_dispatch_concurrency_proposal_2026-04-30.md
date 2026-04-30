# SDDP backward-pass dispatch concurrency — design proposal

**Author:** Claude / 2026-04-30  
**Context:** juan/gtopt_iplp run 2026-04-30 16:23, post-merge of PRs #440-#444.

## Observed behaviour

`[SDDPWorkPool]` stats during iter 2 backward (snapshots every 30 s):

```
16:43:51  Active 16/80  Pending 0  Done 9676
16:44:22  Active 16/80  Pending 0  Done 9676   ← Done unchanged
16:44:54  Active  6/80  Pending 0  Done 9788
16:45:25  Active 22/80  Pending 0  Done 10188
16:45:57  Active  8/80  Pending 0  Done 10652
16:46:28  Active 39/80  Pending 0  Done 10939
16:47:00  Active 48/80  Pending 0  Done 11186
16:47:31  Active 40/80  Pending 0  Done 11450
16:48:01  Active 36/80  Pending 0  Done 11710
```

Concurrently in `top`:
- gtopt %CPU = 1041% (≈10 cores)
- system load avg = 63
- system %us = 92.7% (full utilization at user space, gtopt + other process)

**Pending = 0 always.** No queued tasks — the scheduler is not throttling.
**Active fluctuates 6 → 48** with bursts to 80 not visible at sample
times. The pool is **task-supply-limited**, not worker-limited.

## Throughput is at the structural ceiling

Iter 2 backward in progress, projected wall ≈ 390 s. Pre-merge run iter 2 = 446 s.
**~13% faster** — the per-cell `release_backend` and `drop_cached_primal_only`
changes shaved real time off backward, even though active count dropped.

## Where the dispatch slack comes from

Three structural factors limit how many aperture-LP solves can run simultaneously:

### 1. Cell tasks block on their aperture sub-task futures

In `backward_pass_aperture_phase_impl` and `backward_pass_with_apertures_single_phase`:

```cpp
// inside the per-scene cell worker
auto futures = solve_apertures_for_phase(...);  // submits N aperture tasks
for (auto& f : futures) f.get();                // ← BLOCKS here, holding the slot
install_aperture_backward_cut(...);             // single-thread, on this worker
```

A cell task occupies one pool slot for the entire duration `solve_apertures + install_cut`. With 16 feasible scenes per phase iteration, **16 of 80 pool slots are permanently held by cell tasks waiting on their apertures**. The remaining 64 slots run aperture solves.

Effective worker utilisation: 64/80 = 80 % of the nominal cap.

### 2. Global aperture-clone mutex (`s_global_clone_mutex`)

`sddp_aperture.cpp:194` serialises every `cloneprob` call across all
scenes/phases. Per the comment: "the previous design (per-scene mutex)
crashed with `3 threads GPF'ing at the same IP inside the solver's
shared lib`, fingerprint of concurrent cloneprob without a process-
global lock."

For a single phase iteration: 16 cells × 16 apertures = 256 clones.
Each ~5 ms → **~1.3 s of pure clone serialisation per phase**.
Solve time per aperture is 1-5 s and runs in parallel after clone, so
clone-mutex impact is **small relative to total per-phase wall**, but
visible as the "Active dips to 6" pattern: workers wait in the mutex
queue.

### 3. Per-phase synchronisation barrier

The synchronised backward loop at `sddp_method_iteration.cpp:836-1100`:

```cpp
for phase_index in [N-1 ... 1]:
  for each feasible scene s:
    submit cell task               // 16 tasks
  wait for all cell tasks          // ← barrier
  share_cuts_for_phase(phase-1)    // peer cuts to (s, phase-1)
```

Cut sharing requires the barrier — it reads from every scene's stored
cuts. While waiting at the barrier, late scenes finish while the pool
drains (Active 16 → 6 → 0). Then phase k-1 dispatch starts and Active
ramps up again.

Per-phase gap = (last-task-finish - barrier-cleared-and-next-dispatched).
In the log this looks like ~1-3 s/phase. Over 50 phases × 1-3 s =
**~50-150 s/iteration of pool-idle time**.

## Three candidate optimisations

### Option A — Phase pipelining (overlap consecutive phases)

**Idea:** dispatch phase k-1's scene tasks immediately after phase k's
share_cuts returns, without waiting for cell-task slot drain.

**Hard dependency:** phase k-1's aperture clones source from `(s, k-1)`,
which phase k MUST have fully modified (cut installed + share_cuts'
peer cuts merged) before phase k-1 reads it. Pipelining requires:

- **A.1** Move `install_aperture_backward_cut` out of the cell-task body
  to a sequential post-barrier step. This way the cell task can finish
  earlier, freeing its slot. But the cell task currently does the
  install in the same critical path, so this is a refactor.
- **A.2** Or pipeline only the *aperture-solve* phase of phase k-1
  while phase k's `install_cut` step is still running. This is fragile:
  needs verification that aperture clones don't read state that phase
  k's install hasn't yet committed.

**Verdict:** dependency makes pipelining brittle. The recoverable wall
is ~50-150 s/iteration; not worth a refactor with subtle correctness
risks. **Not recommended.**

### Option B — Per-source-LP clone mutex

**Idea:** replace `static std::mutex s_global_clone_mutex` with one
mutex per source `LinearInterface` (i.e. per `(scene, phase)` cell).
Aperture clones from different `(scene, phase)` source LPs run in
parallel; clones from the same source serialise.

Currently: 256 clones × 5 ms global = 1.3 s/phase serialised.
Proposed: 16 cells × 16 clones × 5 ms parallel = **80 ms/phase**, a
**16× speedup on the clone window**.

**Risk:** the global mutex was added because per-scene mutex crashed
CPLEX with `3 threads GPF'ing at same IP`. The crash signature points
at process-wide CPLEX state (license manager, error stack). Per-source
mutex allows concurrent `CPXcloneprob` with **different source `cpxlp*`
pointers** — which CPLEX docs claim is safe per-env, but the prior
crash suggests there's hidden process-global state.

**Pre-condition for safety:**
1. Audit `CPXcloneprob` for shared-state dependencies on this CPLEX
   version. Specifically: license manager calls, error-stack writes.
2. Stress-test with a synthetic load: 16 cells × 16 apertures cloning
   in parallel for 1000 iterations. Capture any segfault.
3. Verify other backends (HiGHS, MindOpt, OSI) are similarly safe or
   keep the global mutex behind a per-backend flag.

**Estimated win:** 5-10 % on backward wall (~25-40 s/iter on juan).
Limited because clone is already a small fraction of per-phase wall.

**Verdict:** real win but **requires solver-thread-safety audit before
attempting**. Worth doing IF the audit clears CPLEX 22.1.

### Option C — Detached cell tasks (free up the 16 blocked slots)

**Idea:** the cell task currently runs on a pool worker, dispatches
aperture sub-tasks back to the same pool, then blocks on
`future.get()`. The blocked worker holds a slot it isn't using.

**Refactor:** decompose the cell task into three concurrent steps,
none of which blocks a worker on a future:

```cpp
// Phase 1 (pool fan-out): submit aperture tasks DIRECTLY from the
//                         backward-loop coordinator on the main thread.
for each scene s:
  for each aperture a:
    pool.submit(aperture_lambda(s, a));   // 256 tasks for 16 scenes × 16 apertures

// Phase 2 (main thread or coordinator task): collect, install cut.
//                                            Done sequentially per scene
//                                            since install_cut writes to (s, k-1).
for each scene s:
  wait for s's apertures, collect results
  install_aperture_backward_cut(s, ...);    // single-threaded per scene

// Phase 3: barrier, share_cuts, release.
```

**Effect on pool utilisation:**
- Slots held by cell-task `future.get()`: 16 → 0
- Available for aperture solves: 64 → 80
- Theoretical aperture parallelism: +25 %

**Reality check:** the system has only ~10 cores actually available to
gtopt right now (load avg 63, sharing with another process). Going
from 64 to 80 in-flight aperture solves doesn't help when only 10 can
make CPU progress at a time. **The structural slack is real, but the
juan-host CPU contention masks the win.**

On a dedicated host (no co-tenant) the +25 % aperture parallelism
would translate to ~10-20 % faster backward, since aperture solves
saturate compute when given the cores.

**Trade-off:** the refactor moves `install_aperture_backward_cut` from
a pool worker to a coordinator. It's a sequential step per scene with
shared access to `m_cut_store_` (already protected by per-scene
single-writer invariant). Doable but touches the most-tested code path.

**Verdict:** clean architectural improvement, dedicated-host win up
to 20 %, but **shared-host win near zero** under current contention.
Worth doing once the host has fewer co-tenants, OR when scaling beyond
80-thread pools.

## Recommendation

In order of payoff vs. risk:

| Option | Win (juan, shared host) | Win (dedicated) | Risk | Recommend |
|---|---|---|---|---|
| **B** per-source clone mutex | 5-10 % | 5-10 % | Solver-thread-safety risk | **After audit** |
| **C** detached cell tasks | ~0 % | 10-20 % | Refactor risk | **When dedicated** |
| **A** phase pipelining | minimal | minimal | Cut-sharing correctness | **No** |

**Right now**, the juan-shared-host CPU contention is the dominant
bottleneck — gtopt is competing with another process for ~10/20 cores.
Implementing B or C buys little until the host load drops.

**The actionable diagnostic right now:** run on a dedicated host (or
let the co-tenant finish) and re-measure. If gtopt CPU stays around
10 cores even with the host idle, B/C are worth implementing. If
gtopt jumps to 18-20 cores on a quiet host, the previous behaviour
is recovered without code changes.

## What would NOT help (negatives)

- **Bigger pool** (`cpu_factor > 4`): we're already at 80 threads; CPU
  is the bottleneck, not threads.
- **Lower `scheduler_interval`**: the pool isn't gating on schedule
  ticks; `Pending=0` means there's nothing for the scheduler to do.
- **Higher `load_factor`** (currently disabled by default after PR
  #443): this would re-introduce the collective-deadlock pattern from
  the gate-based throttling. Confirmed bad.
- **Force more cell tasks per phase** (e.g. async backward with no
  cut sharing): breaks SDDP convergence semantics.

## If the juan run continues with no host contention…

Look at iter 5+ pool stats. If we still see `Active 36-48/80 Pending 0`
on a quiet host, that's evidence the cell-task block is the structural
ceiling and Option C becomes the right move. If Active climbs to
70-80/80 the moment contention clears, no code change is needed.
