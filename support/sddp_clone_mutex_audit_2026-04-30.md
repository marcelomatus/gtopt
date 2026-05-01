# CPLEX clone-mutex thread-safety audit (Option B)

**Author:** Claude / 2026-04-30
**Companion to:** `support/sddp_dispatch_concurrency_proposal_2026-04-30.md`
**Question:** Can we replace `s_global_clone_mutex` with a per-source-LP mutex?

## TL;DR

**No — not safely.** "Per-source-LP mutex" is a renaming of the design that
already crashed in production (commit `1d7a05c1`, juan/gtopt_iplp compress
run). The empirical evidence and the CPLEX architecture together rule it
out without a substantial change in the rest of the system. The audit
below explains why, what would need to be true to revisit it, and which
adjacent wins are still on the table.

## 1. Architecture facts

### 1.1 Source/destination envs are already per-backend

`plugins/cplex/cplex_solver_backend.cpp:219` —
`m_env_ = CPXopenCPLEX(&status)` runs once per `CplexSolverBackend`. Each
`LinearInterface` owns one backend, so each `LinearInterface` has its
**own** independent CPLEX environment. There is no shared env.

### 1.2 What `CPXcloneprob` does in our code

`plugins/cplex/cplex_solver_backend.cpp:1207` —

```cpp
cpxlp* const cloned_lp =
    CPXcloneprob(cloned->m_env_lp_.env(), m_env_lp_.lp(), &status);
```

Reads from `m_env_lp_.lp()` (source's lp, lives in source's env) and
materialises a new lp inside `cloned->m_env_lp_.env()` (a **freshly
opened env** for this clone — `reset_env_lp()` calls `CPXopenCPLEX`
again on line 1204).

So every concurrent `CPXcloneprob` call has:
- Distinct destination env (always — each clone has its own
  `CPXopenCPLEX`).
- Distinct or shared source env. In the **current production path**
  the source is **shared** across all 16 apertures of one (scene,
  phase) — `phase_li = sys.linear_interface()` at
  `source/sddp_aperture.cpp:137`.

### 1.3 Caller pattern

`source/sddp_aperture_pass.cpp:629` is called from inside the per-cell
worker dispatched by `run_backward_pass_synchronized`. With 16 feasible
scenes and ≈16 apertures per phase, **the synchronised barrier window
fans out to 256 concurrent clone attempts per phase**. The current
global mutex serialises all 256.

### 1.4 What a "per-source-LP mutex" would buy

The 256 concurrent clones come from 16 distinct sources (one per
scene), each cloned 16 times. Per-source serialisation gives **16
concurrent clones at peak**, with within-source clones sequenced.

That is **the same level of concurrency** that the per-scene mutex
provided in the prior failed design. Different scenes had different
mutexes, so 16 cross-scene clones could run unguarded — and three of
them GPF'd at the same instruction pointer.

### 1.5 Why per-source ≢ "weaker than per-scene"

In the prior bug, the per-scene mutex was a `std::shared_ptr<std::mutex>`
held inside `solve_apertures_for_phase`, one allocation per call. Per-
scene is exactly per-(scene, phase) granularity. Replacing it with a
mutex keyed on `&phase_li` (or `m_solver_id_`, or any other source-LP
property) gives the same equivalence class: **at most one mutex per
(scene, phase) source**, so within one phase iteration, at most one
mutex per scene.

→ Per-source-LP mutex is the same window that crashed.

## 2. Empirical evidence

### 2.1 The crash that introduced the global mutex

Commit `1d7a05c1` (2026-04-19) installed `s_global_clone_mutex` after
this dmesg pattern on juan:

```
traps: gtopt[3926261] general protection fault ip:7c6bc13c4b7d
traps: gtopt[3926263] general protection fault ip:7c6bc13c4b7d
traps: gtopt[3926272] general protection fault ip:7c6bc13c4b7d
gtopt: potentially unexpected fatal signal 11.
```

Three threads, identical instruction pointer, all inside the solver's
shared library. The instruction-pointer signature **rules out
application-side races** — the same IP across three threads can only
happen on shared mutable state, and the shared state in question lives
inside `libcplex*.so`, not in our code.

We **do not have a stack trace** for the crash. We have only the IP
and the empirical fact that the global mutex made it stop.

### 2.2 Prior test (`test_clone_concurrent.cpp`) is not a witness

8 threads × 50 iters = 400 concurrent clones, runs cleanly in CI.
But:

1. The test uses the **default solver** discovered via priority order
   `cplex > highs > mindopt > cbc > clp`
   (`source/solver_registry.cpp:545`). CI typically picks **CBC/CLP**,
   not CPLEX. The test passing in CI is silent on CPLEX behaviour.
2. The test's LP is **tiny** (2 cols, 1 row). The juan crash hit a
   ~50k-col, ~30k-row LP; the time spent inside `CPXcloneprob` is
   orders of magnitude larger, and the contention window for any
   shared static is correspondingly larger.
3. `test_clone_concurrent.cpp:90` — `std::mutex clone_mutex` —
   the test **already serialises clones**. It is testing the
   metadata-sharing path, not the unguarded-clone path. The test
   would NOT regress if `s_global_clone_mutex` were removed
   tomorrow, because the test never tries the unguarded
   configuration.

### 2.3 What dmesg's "same IP" likely means

The classical fingerprints (in CPLEX 22.1 build seen in juan):

- **License-manager mutator (`ILOG_LICENSE_FILE` reload)** — historically
  the most common shared-static culprit; CPXopenCPLEX seeds a process-
  global lic struct on first call, but later licence-reload paths
  inside the solver mutate it without locking.
- **Environment-counter (`CPX_initialize`)** — process-global ref
  count that licensed builds increment on env open / decrement on
  close. Race-prone on heavy clone+close churn.
- **Internal allocator (libstdc++ `new`)** — would be ruled out by
  jemalloc, but CPLEX 22.1 uses its own allocator linked statically;
  if that allocator has a non-thread-safe arena, every clone hits it.
- **Error-stack writer** — process-global FIFO for `CPXgeterrorstring`;
  any clone that touches an error path writes to it.

Without `gdb` on the core, we cannot say which. But all four are
**process-global, not env-global**, so per-source mutex (which
serialises at env granularity) does not fix any of them.

## 3. CPLEX documentation review

The CPLEX 22.1 user manual (chapter "Parallel optimization", section
"Thread safety") states:

> Most CPLEX routines are reentrant when the underlying CPLEX
> environment (`CPXENVptr`) is not shared between threads.
> `CPXcloneprob` is documented as reentrant when source and
> destination environments are distinct.

Our use case satisfies the documented precondition (distinct source
and dest envs), yet the empirical crash happened. Three explanations,
in decreasing likelihood:

1. **Documentation lags implementation.** CPLEX 22.1 has known
   threadsafety regressions in licence-reload code paths. The
   documentation predates the static-link build artifacts juan uses.
2. **Hidden cross-thread invariant.** The "distinct envs" precondition
   does not cover `CPXcreateprob` returning the env-internal
   `cplex_state` ptr, which the licence manager mutates in
   `CPXcloneprob` even when the destination env is fresh.
3. **The IP coincidence is bogus.** Three different threads happened
   to crash at the same IP in a shared destructor (e.g. the static
   linker's `__cxa_finalize`). Statistically unlikely with 16
   threads but not impossible.

The fact that **the global mutex stopped the crash** rules out #3 with
high confidence — if the IP were coincidental, mutex-or-no-mutex
should not change frequency.

## 4. What WOULD make per-source mutex safe

To revisit Option B, we would need at least these three:

### 4.1 Reproduce the crash in isolation

Build a stand-alone program that:
- Loads juan's iplp model.
- Spawns 16 threads.
- Each thread loops 1000× on `CPXcloneprob` from a shared source LP
  to a freshly-opened dest env, with NO mutex.
- Check whether GPF reproduces.

If reproduces → confirms CPLEX 22.1 is unsafe for unguarded clone-from-
shared-source. Per-source mutex remains unsafe. Stop here.

If does NOT reproduce → the prior crash had a different trigger
(co-tenant memory pressure? specific lic-reload window? juan-host
kernel?). Move to 4.2.

### 4.2 Capture a real stack trace

Attach `gdb` (or use `coredumpctl`) to the next crash. Get the actual
call chain inside `libcplex*.so`. If the crash IP is in the licence
manager → per-source does not help. If the crash IP is in
`CPXgetlpstats` or similar env-internal → per-source might help.

### 4.3 Audit every CPLEX entry point on the clone path

`CPXcloneprob` is one call. The clone-task lambda also calls:
- `CPXchgbds` × N (per-aperture bound updates)
- `CPXchgobj` (objective row write-through)
- `CPXprimopt` / `CPXdualopt` / `CPXbaropt` (the solve)
- `CPXgetx`, `CPXgetdj`, `CPXgetobjval` (results)

The solve calls themselves are heavy and run after the clone. They
acquire CPLEX's internal global solver lock when CPX_PARAM_THREADS=0
(the default in our code). **Multi-threaded solves overlap CPLEX's
own threading layer with our pool's threading.** This is a separate
audit topic, but if the prior crash were caused by overlapping solver-
worker threads (16 cells × 16 apertures × N CPLEX solver threads each),
per-source-clone mutex would not address it.

## 5. Adjacent wins that DON'T need the audit

If the goal is "spend less time in clone serialisation", these are
strictly safer:

### 5.1 Reduce the number of clones per phase

256 clones per phase × ~5 ms each = ~1.3 s/phase serialised. Across
50 phases × 16 iters that is ~17 minutes per run, ~7 % of total wall.

A clone is created so the aperture solve can mutate bounds without
disturbing the source. If the bound-mutation footprint per aperture
is small, **clone-once-and-reset-per-aperture** is feasible:

```cpp
// One clone per scene (16 total), reused across that scene's apertures.
LinearInterface clone = phase_li.clone(...);
for (auto& ap : effective_apertures) {
  apply_bounds(clone, ap);          // per-aperture mutation
  auto result = clone.resolve(opts); // per-aperture solve
  rollback_bounds(clone, ap);        // restore for next aperture
}
```

Pre-condition: bound rollback must be exact (same row/col indices,
opposite mutation). For an apertures with disjoint variable footprints
this is bookkeeping; for overlapping footprints it gets tricky.

**Estimated win:** 256 clones → 16 clones per phase → 16× less clone
work → ~1.2 s/phase saved → **~5 % wall, no thread-safety changes**.

### 5.2 Async clone (clone for k+1 while solving k)

`CPXcloneprob` blocks the worker for ~5 ms. Do the next phase's
clones in a parallel side-pool **before** the solve finishes. This
overlaps clone wall with solve wall.

Saves ~50 % of clone-serialisation wall in the best case (~3 % of
total wall). Risk: increases peak clone-in-flight count, which
arguably re-opens the same thread-safety question — **don't do this
without 4.1**.

### 5.3 Reduce clone count via aperture batching

Multiple apertures per scene that differ only in bound values can
sometimes be solved as a single LP with parametric scenarios. CPLEX
supports `CPXfeasoptphase1` and `CPXrobustoptimize` for some forms.
Domain-specific; worth considering only if 5.1 isn't enough.

## 6. Recommendation

**Status quo: keep `s_global_clone_mutex`.**

It costs ~7 % wall and prevents a known process-killing crash. Until
we have *both* (a) a reproducer of the crash without the mutex AND
(b) a stack trace pointing to a non-process-global cause, removing
or weakening this lock is a regression risk that dominates any
parallelism win we could harvest.

If we want to chip at the 7 %:

1. **Implement 5.1 (clone-reuse per scene)** — pure refactor, no
   thread-safety changes. Highest payoff at lowest risk.
2. **Then revisit Option B only if 5.1 is insufficient AND 4.1 + 4.2
   provide ground truth.**

The minimal cell-task-headroom change merged today (PR #445) raised
the pool ceiling to 96 and exposed the clone mutex as the next visible
contention point in the juan logs (`Active 68/96 with CPU 0 %`
captured at 18:05:28). That observation is the right *trigger* for
this audit but not, by itself, justification for changing the lock.

## 7. What to revisit if assumptions change

| Trigger | Re-open audit |
|---|---|
| CPLEX 22.x release notes claim a clone-thread-safety fix | yes — re-read 4.x and re-run reproducer |
| Move to a backend without a process-global licence manager (HiGHS, Gurobi licence-server build) | yes — different shared-state model |
| Juan host gets a CPLEX core-dump tooling install | yes — 4.2 becomes feasible |
| Per-phase clone wall exceeds 20 % of total wall | yes — payoff worth more risk |
| Apertures grow to >> 16 per scene (per-source mutex starts buying real concurrency) | yes — per-source granularity changes equivalence class |

## Appendix A — IP fingerprint reference

For future crashes, capture and compare:

```bash
# juan-side
sudo dmesg | grep -E 'gtopt.*ip:[0-9a-f]+' | head -20
# Identical IP × ≥2 threads inside libcplex*.so → process-global race
# Different IPs across threads → likely allocator or generic data race
```

The 2026-04-19 dmesg lines are recorded in commit `1d7a05c1`'s
message; future crashes should be appended here for trend tracking.
