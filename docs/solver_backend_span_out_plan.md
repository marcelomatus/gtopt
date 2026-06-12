# Phase 2 — `SolverBackend` span-out solution API

**Status**: design draft, not yet implemented.
**Phase 1** (commits `0d51bef6`, `522d4150`) cleaned the plugin
caches in place: per-member lazy fills replaced the five-vector
`cache_problem_data()`, HiGHS dropped its solution copy entirely,
and the misleading "no internal solution cache" comment in the LI
was reconciled with reality.  Phase 2 closes the remaining gap.

---

## 1. Problem statement

After Phase 1 the plugin-side state for solutions is:

| Plugin | Solution buffers | Used in off | Used in compress |
|--------|------------------|-------------|------------------|
| OSI    | none — returns `m_solver_->getColSolution()` | direct ptr | direct ptr |
| HiGHS  | none — returns `m_highs_->getSolution().col_value.data()` | direct ptr | direct ptr |
| CPLEX  | `m_col_solution_`, `m_reduced_cost_`, `m_row_price_` | scratch for CPXgetx | **redundant copy**: filled by plugin then immediately re-`assign`-ed into `LinearInterface::m_cached_col_sol_` etc. by `populate_solution_cache_post_solve` |
| MindOpt | same triple | scratch | redundant copy |
| Gurobi  | same triple | scratch | redundant copy |

In compress / rebuild mode a successful solve produces:

```
solver internal state
        ↓ CPXgetx / MDOgetdblattrarray / GRBgetdblattrarray
plugin->m_col_solution_     (≈ ncols × 8 B per cell, transient)
        ↓ memcpy via populate_solution_cache_post_solve
LI->m_cached_col_sol_       (≈ ncols × 8 B per cell, persistent)
```

The plugin buffer is allocated, filled, and then dies on the next
mutation while the LI buffer carries the value forward.  The plugin
buffer in compress/rebuild mode is pure write-once-read-once
overhead.  In off mode the same buffer is the *only* persistent
copy — it is the data source the LI getter falls back to.

## 2. Design — two coexisting APIs

The user-stated constraint is:

> off mode should keep working as now;
> extend the interface for compress mode to pass the buffers,
> e.g. `get_col_sol_raw(buffer)`.

Concretely the plan is:

### 2.1 Add new `fill_*` virtuals on `SolverBackend`

The new methods mirror the LI's `m_cached_col_sol_` / `m_cached_col_cost_`
/ `m_cached_row_dual_` naming so the call sites read fluently as
`fill_col_sol(m_cached_col_sol_)` etc., not `fill_col_solution(m_cached_col_sol_)`
/ `fill_reduced_cost(m_cached_col_cost_)` / `fill_row_price(m_cached_row_dual_)`
where the verb-noun pair changes between layers.

```cpp
class SolverBackend {
public:
  // Existing pointer-returning getters stay — used by off-mode reads
  // and by code paths that want a non-owning view into solver-internal
  // memory (OSI/HiGHS) or the plugin's per-call scratch (CPLEX/etc.).
  // Keep their pre-existing names (matching the C-API of CPLEX,
  // HiGHS, etc.) — only the new span-out variants follow the
  // gtopt/LI naming convention.
  [[nodiscard]] virtual const double* col_solution() const = 0;
  [[nodiscard]] virtual const double* reduced_cost() const = 0;
  [[nodiscard]] virtual const double* row_price()    const = 0;

  // NEW: span-out fills — write the post-solve values directly into
  // a caller-owned buffer.  Used by `populate_solution_cache_post_solve`
  // in compress / rebuild mode so the LI's `m_cached_*` is the
  // sole destination.
  //
  // Naming aligns with the LinearInterface side:
  //   fill_col_sol(out)  → m_cached_col_sol_   (primal solution)
  //   fill_col_cost(out) → m_cached_col_cost_  (reduced cost / col dual)
  //   fill_row_dual(out) → m_cached_row_dual_  (row price / row dual)
  //
  // Default implementation (in this header) memcpy's from the
  // matching pointer-getter, so OSI and HiGHS don't need a dedicated
  // override — the default already moves zero data through the plugin.
  virtual void fill_col_sol (std::span<double> out) const;
  virtual void fill_col_cost(std::span<double> out) const;
  virtual void fill_row_dual(std::span<double> out) const;
};
```

The default in-header implementations:

```cpp
inline void SolverBackend::fill_col_sol(std::span<double> out) const {
  const auto* p = col_solution();          // OSI / HiGHS: live solver pointer
  if (p != nullptr) std::copy_n(p, out.size(), out.data());
}
inline void SolverBackend::fill_col_cost(std::span<double> out) const {
  const auto* p = reduced_cost();
  if (p != nullptr) std::copy_n(p, out.size(), out.data());
}
inline void SolverBackend::fill_row_dual(std::span<double> out) const {
  const auto* p = row_price();
  if (p != nullptr) std::copy_n(p, out.size(), out.data());
}
```

### 2.2 Override on the C-API plugins to write straight to `out`

```cpp
// CPLEX
void CplexSolverBackend::fill_col_sol(std::span<double> out) const {
  if (out.empty()) return;
  CPXgetx(m_env_lp_.env(), m_env_lp_.lp(),
          out.data(), 0, static_cast<int>(out.size()) - 1);
}
void CplexSolverBackend::fill_col_cost(std::span<double> out) const {
  if (out.empty()) return;
  CPXgetdj(m_env_lp_.env(), m_env_lp_.lp(),
           out.data(), 0, static_cast<int>(out.size()) - 1);
}
void CplexSolverBackend::fill_row_dual(std::span<double> out) const {
  if (out.empty()) return;
  CPXgetpi(m_env_lp_.env(), m_env_lp_.lp(),
           out.data(), 0, static_cast<int>(out.size()) - 1);
}
// (analogous for MindOpt MDOgetdblattrarray, Gurobi GRBgetdblattrarray)
```

The plugin's `m_col_solution_/m_reduced_cost_/m_row_price_` members
become **off-mode-only**: they are the write target of the
pointer-getter `col_solution()` (used by the off-mode LI fallback
in `get_col_sol_raw`), and never touched in compress / rebuild mode.

### 2.3 Switch `populate_solution_cache_post_solve` to the span-out API

```cpp
// linear_interface.cpp (Phase 2)
void LinearInterface::populate_solution_cache_post_solve() noexcept {
  if (m_low_memory_mode_ == LowMemoryMode::off) return;  // I6 stays
  // ...
  const auto ncols = static_cast<size_t>(get_numcols());
  const auto nrows = static_cast<size_t>(get_numrows());
  m_cached_col_sol_.resize(ncols);
  m_cached_col_cost_.resize(ncols);
  m_cached_row_dual_.resize(nrows);
  m_backend_->fill_col_sol (m_cached_col_sol_);
  m_backend_->fill_col_cost(m_cached_col_cost_);
  m_backend_->fill_row_dual(m_cached_row_dual_);
  m_cached_is_optimal_ = true;
}
```

Net effect for compress / rebuild:
- One copy per solve (solver → LI buffer) instead of two
  (solver → plugin scratch → LI buffer).
- Plugin's `m_col_solution_/m_reduced_cost_/m_row_price_` stay empty
  in compress / rebuild (they're only sized when off-mode reads
  exercise the pointer-getter).

Net effect for off mode:
- Identical to today.  No new code path; no new LI cache.
- `get_col_sol_raw()` continues to return
  `{backend().col_solution(), get_numcols()}`.

The I6 invariant survives untouched.  The pointer API survives
untouched.  Only the write path inside compress / rebuild changes.

### 2.4 Optional follow-up — eliminate plugin scratch in off mode too

If we later want zero plugin storage in *all* modes, we can:
- Add a span-out variant for `get_col_sol_raw`:
  `void get_col_sol_raw(std::span<double> out) const`.
- Migrate every call site that currently does
  `auto sp = li.get_col_sol_raw(); use(sp);` to provide its own
  buffer.
- Drop the pointer-getter from `SolverBackend` entirely.

That second leg touches every off-mode read site (~5 in
`linear_interface.hpp`, plus their downstream consumers), so it is
explicitly **out of scope** for the Phase 2 commit landed under
this plan.  Phase 2 keeps the pointer API; the optional
follow-up would be a separate, bigger refactor.

---

## 3. Tests

### 3.1 Tests to add **before** the API change (regression guards)

Goal: lock down the *current* solution-read semantics across every
backend in every low-memory mode, so that any silent regression in
Phase 2 fires immediately.

New test file: `test/source/test_solver_backend_solution_contract.cpp`

For each available backend (looped via `SolverRegistry::available_solvers()`):

- **T1.  Pointer-getter contract (off mode).**
  Build a tiny LP, solve, call `col_solution()/reduced_cost()/row_price()`,
  assert the returned values match a known-good vector.  Repeat after
  a mutation+resolve to confirm the buffer is refilled.

- **T2.  Pointer-getter survives one mutation cycle (compress mode).**
  Build → solve → release_backend → ensure_backend → re-solve.
  Read each pointer-getter and assert against expected values.
  This is what `populate_solution_cache_post_solve` does today;
  the test pins the contract explicitly.

- **T3.  No cross-accessor refill.**
  Mock or instrument the C-API call counts (CPLEX: monkey-patch via
  `CPXxsetterminate` or just count callgrind hits; simpler — observe
  via `LinearInterface` profiling counters added for this purpose).
  Assert that calling `col_solution()` triggers exactly one CPXgetx,
  zero CPXgetdj, zero CPXgetpi.

- **T4.  LI cache identity.**  After a successful solve in compress
  mode, assert `&li.get_col_sol_raw()[0] == li.m_cached_col_sol_.data()`
  (via a friend-test accessor) — the LI cache is the canonical
  buffer downstream consumers read.

These tests pass on the current tree (`522d4150`).  They serve as
the safety net for the API change.

### 3.2 Tests to add **after** the API change

New cases in the same file:

- **T5.  `fill_col_sol(span)` matches `col_solution()`.**
  After a solve, allocate two same-sized vectors, fill one via the
  new span-out API and one via copy from the pointer-getter, assert
  element-wise equality.  Same for `fill_col_cost` vs.
  `reduced_cost()` and `fill_row_dual` vs. `row_price()`.

- **T6.  Span-out tolerates short / empty spans.**
  `fill_col_sol(std::span<double>{})` is a no-op; a span of size
  `< get_numcols()` writes only the first `size()` entries (the API
  contract is "fill exactly `out.size()` entries"; over-reading is
  a caller bug — assert via `assert()` in debug, no-op in release).

- **T7.  Compress-mode plugin scratch stays empty.**
  After a solve in compress mode, assert that the plugin's
  `m_col_solution_.empty()` is true (and `m_reduced_cost_/m_row_price_`
  too).  Off-mode counterpart: after a getter call, assert
  `m_col_solution_.size() == get_numcols()`.  Requires a test-only
  accessor on the plugin classes.

- **T8.  Off-mode invariant (I6) preserved.**
  After a solve in off-mode, assert `li.m_cached_col_sol_.empty()`
  (LI cache stays empty — backend is the source of truth).
  Reading `get_col_sol_raw()` returns a span backed by the plugin
  buffer (or solver-internal storage for OSI/HiGHS).

- **T9.  No regression in solution values.**
  T1–T2 already cover this; re-run them after the API change with
  the same tolerance.

### 3.3 Cross-backend equivalence test

- **T10.**  For every available solver, solve the same LP and assert
  that all three accessors agree element-wise (modulo tolerance).
  Catches mistakes where one backend's `fill_*` is ordered or sized
  differently from the others.

---

## 4. Implementation order

1. **Add T1–T4 regression tests** on top of `522d4150`.  Confirm
   they pass.  Commit alone.
2. **Add `fill_col_sol/fill_col_cost/fill_row_dual` to
   `SolverBackend`** with default impl that delegates to the
   pointer-getter.  Override on CPLEX / MindOpt / Gurobi (not OSI
   or HiGHS — defaults are zero-copy already).  Commit alone.
   T1–T4 must still pass.
3. **Switch `populate_solution_cache_post_solve` to use the new
   API.**  Commit alone.  T1–T4 must still pass.
4. **Add T5–T10.**  Commit alone.  All 10 must pass.
5. **Squash if desired** — but keeping each step as a separate
   commit makes bisecting future regressions trivial.

Total estimated diff: ≈ 12 files, ≈ 350 lines.

## 5. Out-of-scope follow-ups (not Phase 2)

- Migrating every off-mode read site to a span-out
  `get_col_sol_raw(out)` API and removing the pointer-getter
  altogether.
- Dropping `m_col_solution_/m_reduced_cost_/m_row_price_` from
  every plugin (would leave the off-mode pointer-getter
  unimplementable on CPLEX/MindOpt/Gurobi — needs option (d) from
  the architecture trade-off discussion).
- Same span-out treatment for problem-data getters
  (`col_lower`/`col_upper`/`obj_coefficients`/`row_lower`/`row_upper`).
  Phase 1 already lazy-split these, so the gain is small.
