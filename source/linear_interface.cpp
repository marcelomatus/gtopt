/**
 * @file      linear_interface.cpp
 * @brief     LinearInterface implementation — solver-agnostic via SolverBackend
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_interface_labels_codec.hpp>
#include <gtopt/lp_equilibration.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

// ── Constructors ──

LinearInterface::LinearInterface(std::unique_ptr<SolverBackend> backend,
                                 std::string plog_file)
    : m_backend_(std::move(backend))
    , m_solver_name_(m_backend_ ? std::string(m_backend_->solver_name())
                                : std::string {})
    , m_solver_version_(m_backend_ ? m_backend_->solver_version()
                                   : std::string {})
    , m_log_file_(std::move(plog_file))
{
  if (m_backend_) {
    m_cached_infinity_ = m_backend_->infinity();
  }
}

LinearInterface::LinearInterface(std::string_view solver_name,
                                 const std::string& plog_file)
    : LinearInterface(
          SolverRegistry::instance().create(
              solver_name.empty() ? SolverRegistry::instance().default_solver()
                                  : solver_name),
          plog_file)
{
}

LinearInterface::LinearInterface(std::string_view solver_name,
                                 const FlatLinearProblem& flat_lp,
                                 const std::string& plog_file)
    : LinearInterface(solver_name, plog_file)
{
  load_flat(flat_lp);
}

// ── Backend lifecycle ──

void LinearInterface::cache_and_release()
{
  // Misnomer retained for back-compat: after the primal/dual cache
  // removal this is a pure "cache scalars post-solve" — the backend is
  // dropped only by an explicit `release_backend()`, never automatically
  // inside resolve/initial_solve.  Callers that need sol/rc must read
  // them after resolve and before release_backend.
  if (m_backend_released_) {
    return;
  }

  if (!m_backend_ || !is_optimal()) {
    return;
  }

  // Cache post-solve scalars so that get_status() / get_obj_value() /
  // get_kappa() / get_numrows() / get_numcols() return the same
  // solve-time values regardless of `low_memory_mode` — the live
  // backend is free to recompute (or silently drop) kappa on every
  // query, which breaks `solution.csv` invariance between `off` and
  // `compress` modes (observed on CLP via OSI: `backend->get_kappa()`
  // returned 5e-18 at solve-time but 0 on a later re-query).
  m_cache_.set_obj_value(m_backend_->obj_value());
  m_cache_.set_kappa(m_backend_->get_kappa());
  m_cache_.set_numrows(get_numrows());
  m_cache_.set_numcols(get_numcols());
  m_cache_.set_is_optimal(/*v=*/true);
}

void LinearInterface::ensure_backend()
{
  if (!m_backend_released_) {
    return;
  }
  // Compress: reconstruct from the saved flat LP snapshot.
  // No warm-start: the barrier method is the default solver algorithm
  // and gains nothing from a starting solution.  The cached col_sol /
  // row_dual remain the source of truth for any pre-solve reader (see
  // `get_col_sol_raw` for the gating logic that prefers the cache
  // until the next resolve refreshes the backend's solution buffer).
  reconstruct_backend();
  // Post-condition mirrors the rebuild branch above.
  if (m_backend_ == nullptr || m_backend_released_) {
    throw std::runtime_error(
        "LinearInterface::ensure_backend: snapshot reconstruct "
        "returned without restoring the backend (m_backend_ is null "
        "or still marked released).  This indicates the snapshot "
        "data is missing or corrupted.");
  }
}

void LinearInterface::release_backend() noexcept
{
  if (m_low_memory_mode_ == LowMemoryMode::off) {
    return;  // No-op: backend stays alive when low_memory is off
  }

  if (m_backend_released_) {
    // Backend already released (e.g. by cache_and_release after resolve).
    // Nothing to do — cached scalars stay, snapshot stays.
    return;
  }

  // Plan §5 — permanent trace instrumentation for the
  // release/reconstruct lifecycle.  `spdlog::trace` (function form
  // — the SPDLOG_TRACE macro is compiled out by the PCH which bakes
  // SPDLOG_ACTIVE_LEVEL=INFO; see feedback_spdlog_trace_macro).
  // Cost is a single atomic level-load when trace is disabled, so
  // this stays unconditional in INFO builds.
  try {
    spdlog::trace(
        "LI release [mode={}]: backend numrows={} numcols={} optimal={} "
        "active_cuts={} pending_coeffs={} pending_rhs={} "
        "pending_coeff_updates_post_revert=0",
        static_cast<int>(m_low_memory_mode_),
        m_backend_ ? get_numrows() : 0,
        m_backend_ ? get_numcols() : 0,
        m_backend_ && is_optimal() ? 1 : 0,
        m_replay_.active_cuts_size(),
        m_replay_.pending_coeffs_size(),
        m_replay_.pending_rhs_size());
  } catch (...) {  // noexcept logging path
  }

  // Cache post-solve scalars for transparent read access.  Full
  // primal/dual vectors are not cached; callers that need them must
  // read before release or trigger ensure_backend() to reload.
  try {
    if (m_backend_ && is_optimal()) {
      // Refresh cached structural counts (always valid post-load_flat,
      // even without a resolve) and re-affirm cached optimality.
      m_cache_.set_numrows(get_numrows());
      m_cache_.set_numcols(get_numcols());
      m_cache_.set_is_optimal(/*v=*/true);

      // Solution-vector refresh is conditional: only re-read the live
      // backend's primal/dual/reduced-cost buffers when there has been
      // a fresh solve since the last reconstruct.  Otherwise the live
      // buffers are uninitialised (post-`load_flat`, pre-resolve) and
      // overwriting the cache with them silently corrupts the SDDP
      // forward/backward read surface — which is exactly the bug that
      // caused juan/iplp compress mode to diverge from `off` mode at
      // iter ≥1 (zero-padded col_sol propagated via
      // `propagate_trial_values`).  When `!backend_solution_fresh()`
      // the existing cache is the source of truth — leave it alone.
      if (m_cache_.backend_solution_fresh()) {
        m_cache_.set_obj_value(m_backend_->obj_value());
        m_cache_.set_kappa(m_backend_->get_kappa());

        // Snapshot primal + dual + reduced-cost vectors directly via
        // the SolverBackend span-out API so that read-only consumers
        // (OutputContext, Benders cut assembly, SDDP state
        // propagation) can access the solution without forcing a
        // backend reconstruct + re-solve.  The fill_* path writes
        // straight into our cache buffer — for CPLEX/MindOpt/Gurobi
        // that's the C-API call into LpCache's vectors, no plugin-
        // side scratch touched; for OSI/HiGHS the default fill
        // memcpy's from the live solver pointer.
        const auto ncols_sz = static_cast<size_t>(m_cache_.numcols());
        const auto nrows_sz = static_cast<size_t>(m_cache_.numrows());
        m_backend_->fill_col_sol(m_cache_.col_sol_buffer(ncols_sz));
        m_backend_->fill_col_cost(m_cache_.col_cost_buffer(ncols_sz));
        m_backend_->fill_row_dual(m_cache_.row_dual_buffer(nrows_sz));
      }
    } else {
      // Non-optimal release: drop any stale primal/dual snapshot so
      // future `get_col_sol()` reads don't return values that belong
      // to a previous LP state (e.g. before `add_row` mutated the
      // system).  Keeps the getter invariant simple: cached vectors
      // are valid iff populated.
      m_cache_.clear_all_solution_vectors();
      // Also flip the cached optimality flag so consumers reading
      // `is_optimal()` post-release see the *current* (non-optimal)
      // status, not a stale `true` left from a prior optimal solve.
      // Without this, `SystemLP::write_out` proceeded past its
      // `if (!is_optimal()) return;` guard on a cell whose backend
      // had been reset, then `OutputContext`'s ctor called
      // `get_col_sol()` which fell through to `backend()` → null
      // unique_ptr deref.  Observed on juan/gtopt_iplp iter i2
      // post-CONVERGED write_out for cells that were optimal in iter
      // i0/i1 but went non-optimal in i2 (status 2 / no-recovery
      // forward failure → release_backend with `is_optimal()==false`).
      m_cache_.invalidate_optimal_on_mutation();
    }
    // Snapshot/compress: first call compresses the flat LP (one-time,
    // creates a persistent buffer); subsequent calls free the decompressed
    // vectors.
    if (!m_snapshot_holder_.is_compressed()) {
      enable_compression();
    } else {
      clear_flat_lp_vectors(m_snapshot_holder_.snapshot_mut().flat_lp);
    }
    // Also compress the label-metadata vectors.  `noexcept` contract
    // on `release_backend` means we swallow any compression error
    // here; next `generate_labels_from_maps` would fall back to
    // whatever is still live (nothing, by design) — acceptable
    // worst-case is `write_lp` throwing on missing metadata, which
    // the caller is already defensive against.
    try {
      compress_labels_meta_if_needed();
    } catch (...) {
      // Best-effort — proceed with release.
    }
  } catch (...) {
    // Best-effort: proceed with release even if caching fails. The
    // function is noexcept, so swallowing exceptions here is intentional —
    // a failure to cache the solution must not break shutdown ordering.
  }

  // Order matters under async iter-overlap: a concurrent reader
  // (e.g. SDDP state-variable propagation -> `get_numcols` -> ...) might
  // see `m_backend_released_ == false` and then dereference
  // `m_backend_`.  Setting the flag BEFORE resetting the pointer
  // narrows the race window to the load of the flag (a CPU-visible
  // store/load barrier moment) — readers who observe `released==false`
  // can still see a non-null `m_backend_`, and readers who observe
  // `released==true` short-circuit to the cache.  The defensive
  // `!m_backend_` check in `get_numcols` covers the residual window
  // where the flag store hasn't propagated to the reader's CPU but
  // the reset has.
  m_backend_released_ = true;
  m_backend_.reset();
  m_phase_ = LiPhase::BackendReleased;
}

// ── Cache invalidation on LP mutation ──
//
// Called from every public LP-mutation API (add_row / add_col /
// set_coeff / set_*_bound / set_obj_coeffs / reset_from / …) so the
// cached optimality flag and cached primal/dual/reduced-cost vectors
// are dropped the moment the LP structurally changes.  This restores
// byte-symmetry between `LowMemoryMode::off` and `compress`:
//
//   * Under `off`, the live backend (CPLEX/HiGHS/...) flips
//     `is_proven_optimal()` to false the instant a row/col or
//     coefficient changes — `physical_eini` / `physical_efin` and
//     other readers gated on `is_optimal()` then correctly fall
//     through to default values until the next `resolve()`.
//
//   * Under `compress`, we'd otherwise carry `m_cached_is_optimal_`
//     forward from the prior optimal solve, surviving a
//     mid-iteration `add_row` (Benders cut), and downstream
//     consumers would silently read a now-stale cached col_sol that
//     doesn't satisfy the new cut.  The SDDP backward pass then
//     produces inconsistent cuts vs off mode — exactly the juan/iplp
//     LB divergence (compress LB ≈ 5× off LB).
//
// Skipped during `m_replaying_` (bulk replay restores recorded
// state — not a new mutation) and under `LowMemoryMode::off` (off
// uses the live backend's flag directly).  Memory is released via
// `shrink_to_fit` so the per-cell footprint after invalidation
// matches off mode's (no cached vectors held).
void LinearInterface::invalidate_cached_optimal_on_mutation() noexcept
{
  if (m_replay_.replaying()) {
    return;
  }
  // Drop the cached optimality flag and primal/dual/reduced-cost
  // vectors regardless of `low_memory_mode`.  Off mode used to skip
  // this branch on the assumption that the live backend's
  // `is_proven_optimal()` already flips false on mutation, but with
  // the LI cache now serving as the single source of truth (post the
  // plugin-cache removal), readers consult the LI cache exclusively
  // when populated — letting it survive a mutation under off would
  // hand stale solution data to downstream consumers (e.g. SDDP
  // state propagation) on the next read after an `add_row`.
  m_cache_.invalidate_optimal_on_mutation();
  m_cache_.clear_all_solution_vectors();
}

void LinearInterface::populate_solution_cache_post_solve() noexcept
{
  // Called from inside `initial_solve` / `resolve` after the backend
  // reports a fresh solve.  Snapshots primal/dual/reduced-cost vectors
  // into the LI cache so downstream readers (OutputContext, Benders
  // cut assembly, SDDP state propagation, write_out, …) all consult
  // the SAME buffer.
  //
  // Plugin-side state: each non-OSI/non-HiGHS backend (CPLEX, MindOpt,
  // Gurobi) holds a small `mutable std::vector<double>` per solution
  // accessor as the C-API write target — the buffer is plain scratch
  // storage with no caching semantics (refilled on every call), and
  // the LI cache below is the single layer that survives across the
  // backend's lifetime.  OSI returns a pointer into the live solver
  // (CLP) and HiGHS into `Highs::getSolution()` — no plugin storage
  // at all in those two.
  //
  // **Off-mode invariant (I6):** under `LowMemoryMode::off` the LI
  // cache MUST stay empty — the live backend is always available and
  // is the sole source of truth; populating a parallel cache would
  // be pure waste (extra alloc + memcpy on every solve, no read-side
  // benefit since backend reads are cheap when the backend is live).
  // Reads under off therefore route directly to `backend()` via the
  // empty-cache fallback in `get_col_sol_raw` / `get_col_cost_raw` /
  // `get_row_dual_raw`.
  //
  // Skip when no backend (defensive) or the just-completed solve was
  // not optimal: in the latter case primal/dual values may be
  // arbitrary and propagating them would corrupt readers that gate on
  // `is_optimal()`.  Cache stays empty until a successful re-solve.
  if (m_low_memory_mode_ == LowMemoryMode::off) {
    return;  // I6: off owns no LI cache
  }
  try {
    if (m_backend_ == nullptr || !m_backend_->is_proven_optimal()) {
      return;
    }
    const auto ncols = static_cast<size_t>(get_numcols());
    const auto nrows = static_cast<size_t>(get_numrows());
    // Span-out fills: backend writes directly into the LI cache
    // buffer without ever touching a plugin scratch (CPLEX/MindOpt/
    // Gurobi C-API call into LpCache vectors) or, for OSI/HiGHS,
    // memcpys from the live solver pointer via the default fill impl.
    // Either way the LI cache is the sole destination — no second copy.
    m_backend_->fill_col_sol(m_cache_.col_sol_buffer(ncols));
    m_backend_->fill_col_cost(m_cache_.col_cost_buffer(ncols));
    m_backend_->fill_row_dual(m_cache_.row_dual_buffer(nrows));
    // Snapshot column bounds too so subsequent `get_col_low_raw` /
    // `get_col_upp_raw` and the `get_col_sol()` clamp path can serve
    // them from the LI cache instead of triggering the backend's
    // pointer-getters (which under CPLEX/MindOpt/Gurobi force
    // allocation of `numcols`-sized scratch vectors `m_collb_` /
    // `m_colub_`).  Same span-out contract as the three solution
    // fillers above — the backend writes directly into the LI buffer.
    m_backend_->fill_col_lower(m_cache_.col_low_buffer(ncols));
    m_backend_->fill_col_upper(m_cache_.col_upp_buffer(ncols));
    m_cache_.set_is_optimal(/*v=*/true);
  } catch (...) {
    // Best-effort: any backend exception leaves the cache in
    // whatever (possibly empty) state it was in.  Caller's
    // `is_optimal()` check will then route through the cached flag
    // and surface a non-optimal status.
  }
}

// ── Low-memory mode ──

void LinearInterface::set_low_memory(LowMemoryMode mode,
                                     CompressionCodec codec) noexcept
{
  m_low_memory_mode_ = mode;
  m_snapshot_holder_.set_codec(codec);

  if (mode == LowMemoryMode::off) {
    // Off mode owns no flat-LP snapshot — the live backend stays
    // alive for the entire run.  Drop any snapshot accumulated under
    // a prior `compress`/`snapshot` configuration.  The LI solution
    // cache (m_cached_col_sol_/_cost_/_dual_) is also reset here so
    // a mode flip from compress→off doesn't carry a stale cached
    // solution forward; it will be re-populated on the next solve
    // by `populate_solution_cache_post_solve` (the LI cache is the
    // single source of truth across all modes after the plugin-cache
    // removal).
    m_snapshot_holder_.clear();
    m_cache_.clear_all_solution_vectors();
    m_cache_.invalidate_optimal_on_mutation();
  }
}

void LinearInterface::freeze_for_cuts(LowMemoryMode mode,
                                      FlatLinearProblem flat_lp,
                                      CompressionCodec codec)
{
  // Pre-conditions: the structural build is done (backend is live or
  // its row count is cached) and no cut has landed yet.  Asserted in
  // debug; production builds short-circuit silently if the caller
  // misuses the API — the legacy three-method dance had no such
  // guards either, so this is strictly a safety upgrade.
  assert(m_replay_.active_cuts().empty()
         && "freeze_for_cuts: cut additions detected before "
            "structural-build commit");

  set_low_memory(mode, codec);
  // Skip the snapshot only for `off` (no reconstruct path).  `compress`
  // (and the legacy `snapshot` alias) genuinely depend on the snapshot
  // to rehydrate the backend.
  if (mode != LowMemoryMode::off) {
    save_snapshot(std::move(flat_lp));
  }
  save_base_numrows();
  m_phase_ = LiPhase::Frozen;
}

void LinearInterface::save_snapshot(FlatLinearProblem flat_lp)
{
  m_snapshot_holder_.snapshot_mut().flat_lp = std::move(flat_lp);
}

void LinearInterface::defer_initial_load(FlatLinearProblem flat_lp)
{
  // Skip the initial load_flat() entirely: stash the snapshot,
  // compress immediately if requested, and mark the backend as
  // released so the next ensure_backend() call performs the (sole)
  // load_flat via reconstruct_backend.
  //
  // Pre-seed the cached row/col counts from the flat LP itself so
  // that callers like `save_base_numrows()` (which read
  // `get_numrows()` while the backend is released) get correct
  // values without having to force a reconstruction.
  m_cache_.set_numrows(flat_lp.nrows);
  m_cache_.set_numcols(flat_lp.ncols);

  m_snapshot_holder_.snapshot_mut().flat_lp = std::move(flat_lp);

  if (m_low_memory_mode_ == LowMemoryMode::compress
      && !m_snapshot_holder_.is_compressed())
  {
    enable_compression();
  }

  // No backend was ever created, so there is nothing to free.
  // Just flip the released flag so ensure_backend() reconstructs lazily.
  m_backend_released_ = true;
}

void LinearInterface::reconstruct_backend()
{
  if (!m_backend_released_ || !m_snapshot_holder_.has_data()) {
    return;
  }

  // Plan §5 trace — record the reconstruct event with replay counts
  // so a `--trace-log` capture can see whether dynamic cols / cuts /
  // coefficient updates are being replayed correctly across cycles.
  spdlog::trace(
      "LI reconstruct [mode={}]: replaying dynamic_cols={} dynamic_rows={} "
      "active_cuts={}",
      static_cast<int>(m_low_memory_mode_),
      m_replay_.dynamic_cols_size(),
      m_replay_.dynamic_rows_size(),
      m_replay_.active_cuts_size());

  // Level 2: decompress the snapshot
  disable_compression();

  // Mark as not released early to avoid recursion in add_col/add_rows
  m_backend_released_ = false;
  // Backend's solution buffers are zero-padded by `load_flat` and
  // `is_proven_optimal()` returns false until the next resolve.
  // Tell `is_optimal()` / `get_col_sol_raw()` to consult the cache.
  m_cache_.mark_solution_fresh(/*v=*/false);
  // Evict stale column-bound snapshots from the previous solve cycle.
  // Under compress, `populate_solution_cache_post_solve()` captures the
  // LP solver's internal column bounds after every forward/backward solve
  // (including any presolve-tightened finite values) into `m_col_low_` /
  // `m_col_upp_`.  The `ReplayGuard` inside `apply_post_load_replay`
  // suppresses `invalidate_cached_optimal_on_mutation()`, so those stale
  // snapshots would otherwise survive the replay cycle and be returned
  // by `get_col_low_raw()` / `get_col_upp_raw()` — causing
  // `apply_alpha_floor`'s `is_pos_inf` / `is_neg_inf` checks to
  // mis-classify a structurally unbounded state-variable column as
  // bounded, which drives the alpha floor deeply negative and collapses
  // the SDDP upper bound (observed: UB goes negative under 2+ scenarios
  // at cascade level transitions with low_memory_mode=compress).
  m_cache_.clear_col_bounds_cache();

  // 1. Reload the base structural LP
  load_flat(m_snapshot_holder_.snapshot().flat_lp);

  // 2. Replay persistent SDDP state onto the live backend.
  apply_post_load_replay(m_replay_);

  // 3. Free decompressed flat LP vectors — the data is now in the backend.
  //    The compressed buffer stays valid as persistent cache for next
  //    reconstruction.  No re-compression needed.
  if (m_snapshot_holder_.is_compressed()) {
    clear_flat_lp_vectors(m_snapshot_holder_.snapshot_mut().flat_lp);
  }
  m_phase_ = LiPhase::Reconstructed;
}

void LinearInterface::apply_post_load_replay(const LpReplayBuffer& source)
{
  // Pre-condition: caller has already run `load_flat` and cleared
  // `m_backend_released_` so the add_col/add_rows below don't re-enter
  // `ensure_backend`.
  assert(!m_backend_released_
         && "apply_post_load_replay requires a live backend");

  // Defence-in-depth flag: bulk `add_cols` / `add_rows` already bypass
  // the single-arg auto-record path, but flipping the replay flag on
  // the OWN buffer here makes the invariant explicit ("replay never
  // grows the persistent registries") and keeps the auto-record
  // gate's failure mode safe even if a future refactor re-routes bulk
  // replay through the single-arg API.  We flip OWN, not source's,
  // so concurrent throwaway clones from the same source don't race
  // on the source's flag.
  const LpReplayBuffer::ReplayGuard replay_guard {m_replay_};

  // 1. Replay dynamic columns (typically just alpha — very few).
  //
  // Uses the bulk `add_cols(span)` so the API mirrors the bulk
  // `add_rows` cut-replay below; semantically equivalent to a per-
  // element `add_col(SparseCol)` loop because the bulk variant
  // dispatches column-by-column (no batched CSR insert is exposed
  // by LP backends for columns).  Each `add_col(SparseCol)` extends
  // `m_col_scales_` when `col.scale != 1.0` — legitimate here
  // because `α` is registered with `scale = scale_alpha`
  // (`sddp_method.cpp:434-453`) and the replay path holds the
  // unique reference to the shared metadata
  // (`detach_for_write(m_col_scales_)` is a no-op).
  if (!source.dynamic_cols().empty()) {
    (void)add_cols(source.dynamic_cols());
  }

  // 2. Replay structural rows that were added after the snapshot was
  //    taken (e.g. cascade elastic-target constraints).  These must
  //    land BEFORE `save_base_numrows()` so that the structural-vs-
  //    cut boundary counts them as structural — otherwise SDDP cut
  //    accounting (m_base_numrows_) is off by `dynamic_rows.size()`
  //    and `record_cut_deletion` indexes the wrong rows.
  if (!source.dynamic_rows().empty()) {
    add_rows(source.dynamic_rows());
  }

  // 3. Mark the structural-vs-cuts boundary.
  save_base_numrows();

  // 4. Bulk-add active cuts (single efficient call).
  if (!source.active_cuts().empty()) {
    add_rows(source.active_cuts());
  }

  // 5. Replay column-bound overrides.  `load_flat` restored the
  //    snapshot's construction-time bounds, but the SDDP forward
  //    pass had pinned dep_col bounds via `propagate_trial_values`
  //    (`source/benders_cut.cpp:91-101`) on the live backend.
  //    Without this replay, iter-0-backward would solve with
  //    construction-time bounds while off mode (which never
  //    reloads) keeps the propagated pins — producing different
  //    cuts and ultimately the juan/iplp compress LB stall.
  //    Uses the bulk `set_col_bounds_raw` API (one CPXchgbds call
  //    on CPLEX, one HighsLp update on HiGHS) instead of N pairs of
  //    `set_col_lower/upper` virtual dispatches — matters on cases
  //    with many forward-pass dep_col pins (production juan/IPLP).
  //    The `ReplayGuard` above already makes the bulk call skip the
  //    per-element `set_pending_col_*` re-record path.
  if (const auto& pending = source.pending_col_bounds(); !pending.empty()) {
    const auto n = pending.size();
    std::vector<ColIndex> idx;
    std::vector<char> lu;
    std::vector<double> values;
    idx.reserve(n * 2U);
    lu.reserve(n * 2U);
    values.reserve(n * 2U);
    for (const auto& [col, bounds] : pending) {
      idx.push_back(col);
      lu.push_back('L');
      values.push_back(bounds.first);
      idx.push_back(col);
      lu.push_back('U');
      values.push_back(bounds.second);
    }
    set_col_bounds_raw(idx, lu, values);
  }

  // 6. Replay raw LP matrix-coefficient overrides issued via
  //    `set_coeff_raw` after the snapshot was taken (HasUpdateLP
  //    elements: ReservoirSeepageLP segment selection,
  //    TurbineProductionFactor, ReservoirDischargeLimit).  Without
  //    this, the aperture path's `clone_from_flat(with_replay=true)`
  //    re-built the LP from the snapshot's construction-time matval
  //    while the source's `update_lp` mutations stayed only on the
  //    pre-release backend → apertures saw construction-time
  //    seepage segment, producing structurally infeasible LPs
  //    against the propagated forward state (juan/gtopt_iplp p51).
  //    Per-(row, col) raw writes — same cost as the original
  //    `set_coeff_raw` mutations they mirror, just re-issued.
  for (const auto& [rc, v] : source.pending_coeffs()) {
    m_backend_->set_coeff(rc.first, rc.second, v);
  }

  // 7. Replay raw LP RHS overrides issued via `set_rhs_raw` after the
  //    snapshot was taken — companion to step 6 for the row-RHS side
  //    of piecewise constraints (seepage `flow = const + slope·v`,
  //    discharge limit, …).
  for (const auto& [r, v] : source.pending_rhs()) {
    m_backend_->set_row_bounds(r, v, v);
  }

  // No warm-start step: the barrier method (default solver algorithm)
  // gains nothing from a starting solution.  Pre-solve readers of
  // `get_col_sol*()` / `get_row_dual*()` consume the cached vectors
  // via the `m_backend_released_` gate in `get_col_sol_raw`.
}

void LinearInterface::record_dynamic_col(SparseCol col)
{
  m_replay_.record_dynamic_col_if_tracked(std::move(col));
}

const SparseColLabel* LinearInterface::col_label_at(ColIndex idx) const noexcept
{
  if (idx < 0) {
    return nullptr;
  }
  const auto i = static_cast<std::size_t>(idx);
  // Frozen flatten-side metadata, indexed in `[0, flatten_col_count())`.
  // Read the live shared vector when populated; an empty live vector
  // means we are between `release_backend` (which clears it) and the
  // next `load_flat` (which repopulates it from the snapshot).  In
  // that window no consumer reads label metadata: `write_lp` requires
  // a loaded backend, which implies `load_flat` already ran.
  if (m_col_labels_meta_) {
    const auto& cm = *m_col_labels_meta_;
    if (i < cm.size()) {
      return &cm[i];
    }
    const auto post_offset = i - cm.size();
    if (post_offset < m_post_flatten_col_labels_meta_.size()) {
      return &m_post_flatten_col_labels_meta_[post_offset];
    }
  }
  return nullptr;
}

const SparseRowLabel* LinearInterface::row_label_at(RowIndex idx) const noexcept
{
  if (idx < 0) {
    return nullptr;
  }
  const auto i = static_cast<std::size_t>(idx);
  if (m_row_labels_meta_) {
    const auto& rm = *m_row_labels_meta_;
    if (i < rm.size()) {
      return &rm[i];
    }
    const auto post_offset = i - rm.size();
    if (post_offset < m_post_flatten_row_labels_meta_.size()) {
      return &m_post_flatten_row_labels_meta_[post_offset];
    }
  }
  return nullptr;
}

bool LinearInterface::update_dynamic_col_lowb(std::string_view class_name,
                                              std::string_view variable_name,
                                              double new_lowb) noexcept
{
  return m_replay_.update_dynamic_col_lowb(class_name, variable_name, new_lowb);
}

bool LinearInterface::update_dynamic_col_bounds(std::string_view class_name,
                                                std::string_view variable_name,
                                                double new_lowb,
                                                double new_uppb) noexcept
{
  return m_replay_.update_dynamic_col_bounds(
      class_name, variable_name, new_lowb, new_uppb);
}

bool LinearInterface::update_dynamic_col_bounds(std::string_view class_name,
                                                std::string_view variable_name,
                                                Uid variable_uid,
                                                double new_lowb,
                                                double new_uppb) noexcept
{
  return m_replay_.update_dynamic_col_bounds(
      class_name, variable_name, variable_uid, new_lowb, new_uppb);
}

// Private helper for `apply_post_load_replay` (the sole legitimate
// internal caller for the bulk cut-replay path).  Keeps the
// `add_rows(m_active_cuts_)` call self-documenting and gives plan
// step 6 of the lifecycle refactor a single audit point for the
// metadata-tracking invariant fixed earlier (see commit 13a0dd55).
// The general-purpose `add_rows(span)` API stays public for
// `check_solvers - add_rows` (cross-backend bulk-add coverage).
//
// `m_active_cuts_` stores PHYSICAL-SPACE rows (built by
// `build_benders_cut_physical`, then captured by `record_cut_row`
// BEFORE `add_row`'s compose_physical transform).  Bulk `add_rows`
// correctly applies compose_physical (col_scale × elem /
// scale_objective / row_max) when post-flatten + col_scales/equilibration
// are active — matching `add_row`'s per-row behavior.  Under
// `--no-scale` (no col_scales, no equilibration) it tail-calls into
// the bulk `add_rows_raw` fast path instead.
//
// Earlier (commit pre-2026-05-01), `add_rows` was the bulk equivalent
// of `add_row_raw` (no compose_physical), causing the juan/gtopt_iplp
// 8.86×/iter LB compounding bug under low_memory_mode=compress because
// every reconstruct replayed wrongly-scaled cut rows.  Fixed by
// teaching `add_rows` to dispatch through compose_physical (same gate
// as `add_row`) and adding the new `add_rows_raw` companion for the
// genuinely LP-raw bulk-insert callers.
void LinearInterface::replay_active_cuts()
{
  if (m_replay_.active_cuts().empty()) {
    return;
  }
  add_rows(m_replay_.active_cuts_mut());
}

void LinearInterface::record_dynamic_row(SparseRow row)
{
  m_replay_.record_dynamic_row_if_tracked(std::move(row));
}

void LinearInterface::record_cut_row(SparseRow row)
{
  m_replay_.record_cut_row_if_tracked(std::move(row));
}

void LinearInterface::record_cut_deletion(std::span<const int> deleted_indices)
{
  m_replay_.record_cut_deletion(deleted_indices,
                                static_cast<int>(m_base_numrows_));
}

// ── Compression control ──

void LinearInterface::disable_compression()
{
  if (m_low_memory_mode_ != LowMemoryMode::compress) {
    return;
  }
  m_snapshot_holder_.decompress_if_compressed();
}

void LinearInterface::enable_compression()
{
  if (m_low_memory_mode_ != LowMemoryMode::compress) {
    return;
  }
  const bool was_first_compress = m_snapshot_holder_.compress_if_uncompressed();

  if (was_first_compress) {
    const auto orig = m_snapshot_holder_.snapshot().compressed_lp.original_size;
    const auto comp = m_snapshot_holder_.snapshot().compressed_lp.data.size();
    const auto ratio =
        comp > 0 ? static_cast<double>(orig) / static_cast<double>(comp) : 0.0;
    SPDLOG_DEBUG(
        "  snapshot compressed: {} -> {} bytes (ratio {:.2f}x, codec {})",
        orig,
        comp,
        ratio,
        codec_name(m_snapshot_holder_.snapshot().compressed_lp.codec));
    static std::once_flag first_log_flag;
    std::call_once(
        first_log_flag,
        [&]
        {
          SPDLOG_INFO(
              "  first snapshot compressed: {} -> {} bytes "
              "(ratio {:.2f}x, codec {})",
              orig,
              comp,
              ratio,
              codec_name(m_snapshot_holder_.snapshot().compressed_lp.codec));
        });
  }
}

// ── Problem name ──

void LinearInterface::set_prob_name(const std::string& pname)
{
  m_backend_->set_prob_name(pname);
}

std::string LinearInterface::get_prob_name() const
{
  return m_backend_->get_prob_name();
}

// ── Log file ──

void LinearInterface::set_log_file(std::string_view plog_file)
{
  m_log_file_ = plog_file;
}

void LinearInterface::close_log_handler()
{
  if (m_log_file_.empty()) {
    return;
  }

  m_backend_->close_log();

  if (m_log_file_ptr_) {
    (void)std::fflush(m_log_file_ptr_.get());
  }
}

void LinearInterface::open_log_handler(const int log_level)
{
  if (m_log_file_.empty()) {
    return;
  }

  if (!m_log_file_ptr_) {
    auto file = std::format("{}.log", m_log_file_);
    m_log_file_ptr_ = log_file_ptr_t(std::fopen(file.c_str(), "ae"));

    if (!m_log_file_ptr_) {
      throw std::runtime_error(std::format(
          "failed to open solver log file {} : errno {}", m_log_file_, errno));
    }
  }

  m_backend_->open_log(m_log_file_ptr_.get(), log_level);
}

// ── Clone & warm start ──

LinearInterface LinearInterface::clone(CloneKind kind) const
{
  // Process-global serialisation of `backend().clone()`.
  //
  // The native clone route (this method) calls into the solver's
  // own `clone()` — `CPXcloneprob` on CPLEX, equivalent on every
  // other backend — which has process-global side effects (env,
  // licence manager, internal allocator) that are not reentrant
  // across threads.  Two failure modes have been observed in
  // production with concurrent calls and no mutex:
  //
  //   * Crashes — three SDDPWorkPool threads GPF'd at the same
  //     instruction pointer inside CPLEX on juan/gtopt_iplp
  //     (commit `1d7a05c1` added a per-callsite mutex on the
  //     aperture path).
  //
  //   * Deadlocks — 10/122 SDDPWorkPool threads parked in
  //     `_M_futex_wait_until` two frames below
  //     `BendersCut::elastic_filter_solve`, the SDDP forward pass
  //     wedged for >7 min (juan thread dump, 2026-05-07).
  //
  // The aperture path's per-callsite mutex was insufficient: it
  // only serialised within one entry-point.  Concurrent elastic
  // clones in the forward pass had no protection at all.  Putting
  // the mutex here, inside `LinearInterface::clone()` itself,
  // guarantees every native-clone caller (elastic, aperture,
  // future paths) is automatically safe with no per-call boiler-
  // plate.  Callers that want the parallel-safe manual route
  // (no mutex, builds via `CPXcreateprob` + `CPXaddrows` on a
  // freshly-opened env) call `clone_from_flat()` explicitly when
  // they want **snapshot-state** semantics — that path is for
  // callers who don't need the post-snapshot dynamic state
  // (alpha, installed cuts) replayed into the clone.
  static std::mutex s_native_clone_mutex;
  const std::scoped_lock native_clone_lock(s_native_clone_mutex);

  // Route through the centralized `backend()` accessor so the source
  // auto-resurrects if released (low_memory_mode != off).  Prior to
  // this, direct `m_backend_->clone()` null-deref segfaulted when the
  // SDDP forward pass had released the phase backend before the
  // backward/aperture pass cloned it.
  //
  // The backend is always cloned independently — solver internals
  // (CPLEX env, HiGHS basis factorisation) are non-reentrant.  Only
  // the metadata structures are subject to `CloneKind` dispatch.
  auto cloned = LinearInterface {backend().clone(), m_log_file_};
  cloned.m_scale_objective_ = m_scale_objective_;
  // Propagate the LP-external objective constant (physical units)
  // so the clone's `get_obj_value()` reports the same algebraic
  // value as the source after re-solve.  Without this, an SDDP
  // clone that solves under the substituted formulation would
  // under-report the obj-value by the substitution offset.
  // Propagate raw-scale obj constant so the clone's
  // `get_obj_value_raw()` matches the source after re-solve.
  cloned.m_obj_constant_raw_ = m_obj_constant_raw_;
  // SOS2 set count (issue #504).  The cloned backend (CPXcloneprob)
  // already carries the SOS2 declarations natively, but the LI-side
  // counter must follow it explicitly — without this the
  // ``sos2_set_count()`` accessor returns 0 on every native clone
  // even though the cloned solver state is correct.  ``clone_from_flat``
  // re-runs ``load_flat`` which repopulates the counter from
  // ``FlatLinearProblem::sos2_sets`` and therefore does not need this
  // line.
  cloned.m_sos2_set_count_ = m_sos2_set_count_;
  cloned.m_equilibration_method_ = m_equilibration_method_;
  cloned.m_base_numrows_ = m_base_numrows_;
  cloned.m_base_numrows_set_ = m_base_numrows_set_;
  cloned.m_log_tag_ = m_log_tag_;
  // Propagate label-maker configuration so clone-side `add_col` /
  // `add_row` honour the source's `LpNamesLevel`.  Without this the
  // default-constructed LabelMaker silently drops to `LpNamesLevel::none`
  // on the clone, breaking name tracking for cuts / slacks added after
  // cloning even when the source had `LpNamesLevel::all`.  The
  // `clone_from_flat` route gets this for free via `load_flat`.
  cloned.m_label_maker_ = m_label_maker_;
  // Propagate validation thresholds; stats are intentionally fresh on
  // the clone so each LP tracks only the writes that land on it.
  cloned.m_validation_options_ = m_validation_options_;
  // Carry the structural label metadata across so that
  // ``write_lp`` on the clone can synthesise column/row labels for
  // post-mortem diagnostics (e.g. SDDP elastic-clone dumps when the
  // relaxed LP is itself infeasible — see sddp_forward_pass.cpp:641).
  // Without these, ``generate_labels_from_maps`` throws when the
  // first inherited column is asked to produce a label.

  // CloneKind dispatch — per shared_ptr<T> member.  `deep` value-copies
  // each underlying T into a fresh shared_ptr; `shallow` does an atomic
  // incref so source and clone share state.
  if (kind == CloneKind::deep) {
    cloned.m_variable_scale_map_ = deep_copy_ptr(m_variable_scale_map_);
    cloned.m_col_scales_ = deep_copy_ptr(m_col_scales_);
    cloned.m_row_scales_ = deep_copy_ptr(m_row_scales_);
    cloned.m_col_cost_scale_types_ = deep_copy_ptr(m_col_cost_scale_types_);
    cloned.m_row_cost_scale_types_ = deep_copy_ptr(m_row_cost_scale_types_);
    cloned.m_col_labels_meta_ = deep_copy_ptr(m_col_labels_meta_);
    cloned.m_row_labels_meta_ = deep_copy_ptr(m_row_labels_meta_);
    cloned.m_col_names_ = deep_copy_ptr(m_col_names_);
    cloned.m_row_names_ = deep_copy_ptr(m_row_names_);
    cloned.m_col_index_to_name_ = deep_copy_ptr(m_col_index_to_name_);
    cloned.m_row_index_to_name_ = deep_copy_ptr(m_row_index_to_name_);
  } else {  // shallow — atomic incref, source and clone share state
    cloned.m_variable_scale_map_ = m_variable_scale_map_;
    cloned.m_col_scales_ = m_col_scales_;
    cloned.m_row_scales_ = m_row_scales_;
    cloned.m_col_cost_scale_types_ = m_col_cost_scale_types_;
    cloned.m_row_cost_scale_types_ = m_row_cost_scale_types_;
    cloned.m_col_labels_meta_ = m_col_labels_meta_;
    cloned.m_row_labels_meta_ = m_row_labels_meta_;
    cloned.m_col_names_ = m_col_names_;
    cloned.m_row_names_ = m_row_names_;
    cloned.m_col_index_to_name_ = m_col_index_to_name_;
    cloned.m_row_index_to_name_ = m_row_index_to_name_;
  }

  // Post-flatten metadata is per-instance (never shared via shared_ptr
  // because each clone records its own disposable additions
  // independently in `m_post_clone_*_metas_`).  But the structural
  // post-flatten history that was already on the source at clone time
  // (alpha, cut rows, cascade elastic constraints) MUST be visible to
  // the clone's `write_lp` / `row_label_at`, otherwise label generation
  // throws on those rows.  Value-copy is cheap: post-flatten size is
  // typically O(handful) on aperture-clone paths (1 alpha + a few
  // installed cuts).  Both deep and shallow clones share the same
  // semantics here — the per-clone divergence happens later when the
  // clone's own `add_col_disposable` / `add_row_disposable` writes
  // land in `m_post_clone_*_metas_`, never back into these vectors.
  cloned.m_post_flatten_col_labels_meta_ = m_post_flatten_col_labels_meta_;
  cloned.m_post_flatten_row_labels_meta_ = m_post_flatten_row_labels_meta_;
  cloned.m_post_flatten_col_meta_index_ = m_post_flatten_col_meta_index_;
  cloned.m_post_flatten_row_meta_index_ = m_post_flatten_row_meta_index_;

  return cloned;
}

LinearInterface LinearInterface::clone_from_flat(CloneKind kind) const
{
  // Pre-conditions: the manual route reads from
  // `m_snapshot_holder_.snapshot().flat_lp`, which is only populated when
  // `freeze_for_cuts` ran with `LowMemoryMode::compress` (or `snapshot`).
  // The SDDP aperture path decompresses around the backward pass via
  // `DecompressionGuard` (sddp_aperture_pass.cpp:460 / 693), so callers
  // reaching this method during the aperture window are guaranteed a
  // hydrated `flat_lp.matbeg` even though the persistent compressed
  // buffer (`m_snapshot_holder_.is_compressed()`) may still report
  // ``true`` — the buffer is kept alive as a cache by
  // `LowMemorySnapshot::decompress` and survives the rehydrate.  The
  // check below is therefore on the *accessibility* of the flat LP
  // (matbeg populated), not on the secondary compressed cache.
  if (!m_snapshot_holder_.has_data()) {
    throw std::runtime_error(
        "LinearInterface::clone_from_flat: source has no snapshot — call "
        "freeze_for_cuts(LowMemoryMode::compress) on the source first, or "
        "fall back to clone(kind) which uses the backend's native clone.");
  }
  if (m_snapshot_holder_.snapshot().flat_lp.matbeg.empty()) {
    throw std::runtime_error(
        "LinearInterface::clone_from_flat: source flat LP is not "
        "decompressed (matbeg is empty while compressed cache may be "
        "present); decompress first (DecompressionGuard) or fall back "
        "to clone(kind).");
  }

  // Build the clone via load_flat — no backend.clone() call, no
  // process-global solver side effects, no mutex needed.
  LinearInterface cloned {m_solver_name_, m_log_file_};
  cloned.load_flat(m_snapshot_holder_.snapshot().flat_lp);

  // Copy the LI-side runtime fields that load_flat doesn't restore.
  // These are normally inherited by the native clone() path on the
  // line that does `cloned.m_scale_objective_ = m_scale_objective_`
  // etc.  Mirror that block here so the two clone routes produce
  // equivalent state for downstream consumers.
  cloned.m_base_numrows_ = m_base_numrows_;
  cloned.m_base_numrows_set_ = m_base_numrows_set_;
  cloned.m_log_tag_ = m_log_tag_;
  // `load_flat` already set `m_label_maker_` from the snapshot, but the
  // source may have changed it via `set_label_maker` after load.  Reapply
  // the source's CURRENT setting so both clone routes are symmetric.
  cloned.m_label_maker_ = m_label_maker_;
  cloned.m_validation_options_ = m_validation_options_;

  // CloneKind dispatch — the metadata side.
  //
  // After `load_flat` the clone owns a freshly-built, value-equal
  // copy of every shared_ptr metadata field (rebuilt from the
  // flat's own embedded copies).  That matches `CloneKind::deep`
  // semantics already, so for `deep` we leave the freshly-loaded
  // metadata in place.
  //
  // For `shallow` we re-link the clone's shared_ptrs to point at
  // the source's pointees — same atomic-incref behaviour as the
  // native shallow clone path — so peak-aperture metadata memory
  // stays at one shared copy across N concurrent clones rather
  // than N independent copies.
  if (kind == CloneKind::shallow) {
    cloned.m_variable_scale_map_ = m_variable_scale_map_;
    cloned.m_col_scales_ = m_col_scales_;
    cloned.m_row_scales_ = m_row_scales_;
    cloned.m_col_cost_scale_types_ = m_col_cost_scale_types_;
    cloned.m_row_cost_scale_types_ = m_row_cost_scale_types_;
    cloned.m_col_labels_meta_ = m_col_labels_meta_;
    cloned.m_row_labels_meta_ = m_row_labels_meta_;
    cloned.m_col_names_ = m_col_names_;
    cloned.m_row_names_ = m_row_names_;
    cloned.m_col_index_to_name_ = m_col_index_to_name_;
    cloned.m_row_index_to_name_ = m_row_index_to_name_;
  }

  // Current-state replay: borrow the source's replay buffer and
  // re-apply it so the clone matches the SOURCE's live backend state
  // (post-snapshot α col, installed cuts, forward-pass-pinned bounds,
  // raw coefficient/RHS overrides) instead of the snapshot-frozen
  // state.  This gives elastic / forward-pass callers a parallel-safe
  // equivalent of `clone()` without needing the native CPXcloneprob
  // mutex.
  //
  // The `add_cols` / `add_rows` calls inside `apply_post_load_replay`
  // register their own post-flatten metadata entries on the clone, so
  // we deliberately do NOT copy `m_post_flatten_*_metas_` from the
  // source here — copying first would double-register every entry,
  // tripping the duplicate-detection guard in `track_col_label_meta`
  // and producing observable infeasibility on elastic / aperture
  // clones (the 2026-05-08 "relaxed clone infeasible" regression).
  //
  // The borrow is intentionally read-only — the source's `m_replay_`
  // must stay intact for its own future replays (e.g. the next
  // `release_backend` → `reconstruct_backend` cycle).
  cloned.apply_post_load_replay(m_replay_);
  // After the initial replay, the clone is a "throwaway" instance: it
  // is owned by one chunk task and destroyed at end-of-task; its
  // replay buffer is never re-applied to any backend.  Subsequent
  // mutations (e.g. dense `update_aperture` bound writes in the
  // aperture inner loop) don't need to be recorded.  Flipping this
  // flag short-circuits the per-mutation recording in
  // `set_col_low/upp_raw`, `add_col`, etc.
  cloned.m_is_throwaway_clone_ = true;

  // Stats are post-construction counters — bounds/coefficients written
  // by `load_flat` and `apply_post_load_replay` are reconstruction
  // events, not new observations; the source already counted them.
  // Reset so the clone tracks only writes that land on it post-clone.
  // Mirrors the plugin route (`clone()`), where `backend().clone()`
  // bypasses `note_*` entirely and the freshly-constructed clone's
  // counters start at zero by definition.
  cloned.m_validation_stats_ = {};

  return cloned;
}

// ── Load ──

void LinearInterface::load_flat(const FlatLinearProblem& flat_lp)
{
  // Recreate backend if it was released (low-memory mode reconstruction).
  if (!m_backend_) {
    m_backend_ = SolverRegistry::instance().create(m_solver_name_);
  }

  // Keep the cached infinity in sync with whatever backend is live now.
  m_cached_infinity_ = m_backend_->infinity();

  m_backend_->set_prob_name(flat_lp.name);

  // Count every backend load_problem call so the end-of-run report can
  // distinguish normal-mode (1×) from low-memory reconstruction (N×).
  ++m_solver_stats_.load_problem_calls;

  m_backend_->load_problem(flat_lp.ncols,
                           flat_lp.nrows,
                           flat_lp.matbeg.data(),
                           flat_lp.matind.data(),
                           flat_lp.matval.data(),
                           flat_lp.collb.data(),
                           flat_lp.colub.data(),
                           flat_lp.objval.data(),
                           flat_lp.rowlb.data(),
                           flat_lp.rowub.data());

  // Preserve global objective scale factor from flatten().
  m_scale_objective_ = flat_lp.scale_objective;

  // Preserve the LP-external objective constant (LP raw scale) from
  // the flat LP.  `LinearProblem::add_obj_constant` accumulates in
  // physical units; `flatten()` divides by `scale_objective` so the
  // value stored here is already on the same raw basis as the
  // solver's reported value — `get_obj_value_raw()` then composes
  // them with a plain add.
  m_obj_constant_raw_ = flat_lp.obj_constant_raw;

  // Preserve per-column scale factors from LinearProblem.
  detach_for_write(m_col_scales_)
      .assign(flat_lp.col_scales.begin(), flat_lp.col_scales.end());

  // Preserve per-row equilibration scale factors (empty when disabled).
  detach_for_write(m_row_scales_)
      .assign(flat_lp.row_scales.begin(), flat_lp.row_scales.end());

  // Preserve per-column / per-row objective time-basis (Power / Energy /
  // Raw) so OutputContext can choose the inverse cost-factor family per
  // element when reading reduced costs / duals back to physical units.
  detach_for_write(m_col_cost_scale_types_)
      .assign(flat_lp.col_cost_scale_types.begin(),
              flat_lp.col_cost_scale_types.end());
  detach_for_write(m_row_cost_scale_types_)
      .assign(flat_lp.row_cost_scale_types.begin(),
              flat_lp.row_cost_scale_types.end());

  // Persist the equilibration method so the follow-up PR that migrates
  // Benders cut construction to physical accessors can apply the same
  // per-row scaling to post-build cut rows as the structural build did.
  m_equilibration_method_ = flat_lp.equilibration_method;

  // Preserve VariableScaleMap for dynamic column auto-scaling.
  // `detach_for_write` is a no-op here in practice (load_flat runs
  // on the source LP before any clones exist), but the helper
  // guarantees correctness if a future caller reorders the lifecycle.
  detach_for_write(m_variable_scale_map_) = flat_lp.variable_scale_map;

  // Preserve LabelMaker so dynamically added cols/rows after load_flat()
  // use the same LpNamesLevel as the original flatten() call.
  m_label_maker_ = flat_lp.label_maker;

  // Preserve label-only metadata for every structural col/row so
  // `generate_labels_from_maps` can synthesise real gtopt labels on
  // demand (e.g. at `write_lp` time) without requiring names to have
  // been enabled at flatten.  The overhead is comparable to
  // `colnm`/`rownm` but carries structured fields rather than already-
  // formatted strings.
  //
  // `load_flat` is the SOLE writer of the frozen flatten-side metadata
  // vectors.  Post-`load_flat`, every `add_col` / `add_row` writes into
  // `m_post_flatten_*_labels_meta_` instead, leaving the frozen vectors
  // structurally invariant for the lifetime of the LinearInterface (or
  // until the next `load_flat` for a new structural build).  Clear
  // the post-flatten vectors here so a reload (compress reconstruct)
  // starts from a clean slate; `apply_post_load_replay` re-populates
  // them from
  // `m_dynamic_cols_` / `m_active_cuts_` immediately after load_flat
  // returns.
  detach_for_write(m_col_labels_meta_) = flat_lp.col_labels_meta();
  detach_for_write(m_row_labels_meta_) = flat_lp.row_labels_meta();
  m_post_flatten_col_labels_meta_.clear();
  m_post_flatten_row_labels_meta_.clear();
  m_post_flatten_col_meta_index_.clear();
  m_post_flatten_row_meta_index_.clear();

  // The flat LP carries the authoritative structural labels.  Drop any
  // stale compressed buffer left over from a previous release_backend()
  // cycle so `ensure_labels_meta_decompressed()` doesn't later try to
  // overlay outdated metadata on top of the freshly-loaded vectors.
  m_col_labels_meta_compressed_ = {};
  m_row_labels_meta_compressed_ = {};
  m_col_labels_meta_count_ = 0;
  m_row_labels_meta_count_ = 0;

  // Preserve coefficient statistics computed during flatten().
  m_stats_nnz_ = flat_lp.stats_nnz;
  m_stats_zeroed_ = flat_lp.stats_zeroed;
  m_stats_max_abs_ = flat_lp.stats_max_abs;
  m_stats_min_abs_ = flat_lp.stats_min_abs;
  m_stats_max_col_ = flat_lp.stats_max_col;
  m_stats_min_col_ = flat_lp.stats_min_col;
  m_stats_max_col_name_ = flat_lp.stats_max_col_name;
  m_stats_min_col_name_ = flat_lp.stats_min_col_name;
  m_row_type_stats_ = flat_lp.row_type_stats;

  for (auto i : flat_lp.colint) {
    m_backend_->set_integer(i);
  }

  // SOS2 sets (issue #504 L-secant chord).  Forward each set to the
  // backend after the integer-column flips so that columns
  // participating in SOS2 land in the backend with their final
  // continuous/integer type before the SOS declaration references
  // them.  Backends without SOS2 (CBC/OSI, default-throw) raise a
  // structured error from SolverBackend::add_sos2 — see that
  // method's docstring for the support matrix.  Track the count
  // unconditionally for ``sos2_set_count()`` so the issue #504
  // tests can observe the schema → LP-build → backend forward path
  // without invoking a solver-specific SOS2 query API.
  m_sos2_set_count_ = 0;
  for (const auto& set : flat_lp.sos2_sets) {
    if (set.size() < 2) {
      continue;
    }
    m_backend_->add_sos2(std::span<const int> {set.data(), set.size()});
    ++m_sos2_set_count_;
  }

  // Build name maps — clear first so reconstruction doesn't accumulate
  // duplicates from a previous load_flat() call.
  auto build_name_map = []<typename IndexType>(const auto& names_vec,
                                               auto& name_map,
                                               auto& index_to_name)
  {
    name_map.clear();
    index_to_name.assign(names_vec.begin(), names_vec.end());
    name_map.reserve(names_vec.size());

    for (const auto [i, name] : enumerate<IndexType>(names_vec)) {
      if (!name.empty()) {
        name_map.emplace(name, i);
      }
    }
  };

  if (!flat_lp.colnm.empty()) {
    build_name_map.template operator()<ColIndex>(
        flat_lp.colnm,
        detach_for_write(m_col_names_),
        detach_for_write(m_col_index_to_name_));
  }

  if (!flat_lp.rownm.empty()) {
    build_name_map.template operator()<RowIndex>(
        flat_lp.rownm,
        detach_for_write(m_row_names_),
        detach_for_write(m_row_index_to_name_));
  }
}

// ── Time limit ──

void LinearInterface::set_time_limit(double /*time_limit*/)
{
  // Time limit is now handled via apply_options() in the backend.
  // This method is kept for API compatibility but is a no-op.
  // Use SolverOptions::time_limit instead.
}

// ── Column operations ──

ColIndex LinearInterface::add_col(const std::string& name,
                                  double collb,
                                  double colub)
{
  // Ensure the backend is live before we touch it.  Under
  // `LowMemoryMode::compress`, `m_backend_` may have been released;
  // `ensure_backend()` lazily reconstructs it from the snapshot.  The
  // assert doubles as a hint to clang-static-analyzer, which otherwise
  // can't see through the reconstruct path and flags every subsequent
  // `m_backend_->…` as a potential null dereference.
  ensure_backend();
  assert(m_backend_ != nullptr);
  const auto index = m_backend_->get_num_cols();
  const auto col = ColIndex {index};

  // Validation hooks (Phase 2): note each finite bound; sentinels
  // (±solver_infinity) are skipped by `note_bound` itself via its
  // `std::isfinite` guard, but the explicit `solver_infinity` cap
  // also catches DblMax-style sentinels passed from CLI tests.
  // note_bound's deferred-format overload skips the std::format
  // allocation entirely on the hot path: every well-conditioned bound
  // returns at the threshold check before touching `col`.
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(collb) < infy_guard) {
      m_validation_stats_.note_bound(collb, col, m_validation_options_);
    }
    if (std::abs(colub) < infy_guard) {
      m_validation_stats_.note_bound(colub, col, m_validation_options_);
    }
  }

  m_backend_->add_col(normalize_bound(collb), normalize_bound(colub), 0.0);
  invalidate_cached_optimal_on_mutation();

  // Uniqueness is enforced on metadata (via the build-time
  // LinearProblem dedup and the post-flatten dedup index) in
  // add_col(SparseCol).  The string path here just records the
  // pre-formatted name when one is provided; duplicates surface at
  // the metadata layer.
  if (m_label_maker_.col_names_enabled() && !name.empty()) {
    auto& cin = detach_for_write(m_col_index_to_name_);
    if (std::ssize(cin) <= index) {
      cin.resize(static_cast<size_t>(index) + 1);
    }
    cin[col] = name;
  }

  return col;
}

ColIndex LinearInterface::add_col(const std::string& name)
{
  return add_col(name, 0.0, m_backend_->infinity());
}

ColIndex LinearInterface::add_free_col(const std::string& name)
{
  return add_col(name, -m_backend_->infinity(), m_backend_->infinity());
}

ColIndex LinearInterface::emit_col_to_backend(const SparseCol& col)
{
  ensure_backend();

  const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);
  const auto index = m_backend_->get_num_cols();

  // Validation hooks (Phase 2): note bounds + objective coefficient.
  // Pass the strong column index directly — note_* overloads format
  // it lazily (only on the cold "huge" / "tiny" branch).
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    const auto cidx = ColIndex {index};
    if (std::abs(col.lowb) < infy_guard) {
      m_validation_stats_.note_bound(col.lowb, cidx, m_validation_options_);
    }
    if (std::abs(col.uppb) < infy_guard) {
      m_validation_stats_.note_bound(col.uppb, cidx, m_validation_options_);
    }
    m_validation_stats_.note_obj(col.cost, cidx, m_validation_options_);
  }

  // Apply col.scale and scale_objective multiplicatively, matching
  // `LinearProblem::flatten`'s convention for structural columns
  // and `add_row`'s compose_physical path for cuts:
  //   raw_cost = col.cost × col.scale / scale_objective
  //
  // Callers pass the PHYSICAL cost — col.scale is folded in here
  // (unlike the previous pre-scale requirement that forced callers
  // to know the effective col.scale, which is impossible when ruiz
  // equilibration adds a factor on top of the user-declared scale).
  m_backend_->add_col(lowb, uppb, col.cost * col.scale / m_scale_objective_);
  invalidate_cached_optimal_on_mutation();

  return ColIndex {index};
}

void LinearInterface::track_col_label_meta(ColIndex col_idx,
                                           const SparseCol& col)
{
  // Track label-only metadata so `generate_labels_from_maps` can
  // synthesise the label on demand at `write_lp` time.  No eager
  // `LabelMaker::make_col_label` call — the formatted string is
  // produced exclusively in `generate_labels_from_maps`, cached
  // there, and reused on subsequent `write_lp` invocations
  // (Option B: always lazy + cache on first compute).
  //
  // Post-flatten path: the frozen `m_col_labels_meta_` (set ONCE by
  // `load_flat`) is intentionally left untouched here.  Every column
  // added after `load_flat` lands in the per-instance
  // `m_post_flatten_col_labels_meta_` vector; lookup via
  // `col_label_at(idx)` walks `[frozen | post-flatten]` and resolves
  // the right side based on `idx < flatten_col_count()`.  This skips
  // the eager `ensure_labels_meta_decompressed` round-trip that the
  // previous in-place-mutation path needed (training / SDDP never
  // formats strings, so the frozen side stays compressed end-to-end
  // unless `write_lp` actually consumes it).
  const auto i = static_cast<size_t>(col_idx);
  const auto frozen_count = flatten_col_count();
  if (i < frozen_count) {
    // Re-tracking a frozen flatten-side column.  Should not happen on
    // any production path (load_flat is the sole writer of frozen
    // metadata and bypasses `track_col_label_meta` entirely), but a
    // few tests / disposable paths still emit cols at indices < the
    // frozen prefix; treat those as a no-op so the frozen invariant
    // holds.
    return;
  }
  const auto post_offset = i - frozen_count;
  if (m_post_flatten_col_labels_meta_.size() <= post_offset) {
    m_post_flatten_col_labels_meta_.resize(post_offset + 1);
  }
  m_post_flatten_col_labels_meta_[post_offset] = SparseColLabel {
      .class_name = col.class_name,
      .variable_name = col.variable_name,
      .variable_uid = col.variable_uid,
      .context = col.context,
  };

  // Eager metadata-based duplicate detection.  Key is the metadata
  // slot just written into `m_post_flatten_col_labels_meta_`; no
  // separate label is constructed.  Unlabelled cols (no class_name
  // / no variable_name / unknown uid / monostate context) are
  // skipped so structural tests that build unnamed cells don't
  // collide.  The check consults the per-instance
  // `m_post_flatten_col_meta_index_` — duplicate post-flatten
  // insertions against any previously inserted post-flatten entry
  // on this instance are reported.
  const auto& meta = m_post_flatten_col_labels_meta_[post_offset];
  if (!is_empty_col_label(meta)) {
    auto [it, inserted] =
        m_post_flatten_col_meta_index_.try_emplace(meta, col_idx);
    if (!inserted) {
      throw std::runtime_error(
          std::format("Duplicate LP column metadata: class='{}' var='{}' "
                      "uid={} (first at col {}, duplicate at col {})",
                      meta.class_name,
                      meta.variable_name,
                      meta.variable_uid,
                      it->second,
                      col_idx));
    }
  }
}

ColIndex LinearInterface::add_col(const SparseCol& col)
{
  const auto col_idx = emit_col_to_backend(col);

  // Register scale if non-unity.
  if (col.scale != 1.0) {
    set_col_scale(col_idx, col.scale);
  }

  track_col_label_meta(col_idx, col);

  // Auto-record post-init mutations so they are replayed on the next
  // `release_backend` → `reconstruct_backend` cycle under compress.
  // Skips when `m_replaying_` is set (apply_post_load_replay's bulk
  // re-entry — the bulk path bypasses this single-arg variant anyway,
  // but the flag is a defence-in-depth guard).
  //
  // Post-init gate: `m_snapshot_holder_.has_data()` flips true once
  // `defer_initial_load` (or `freeze_for_cuts`) installs the flat
  // LP, which is precisely the structural-build → cut-build
  // boundary.  Pre-snapshot structural builds still go straight
  // to the backend without growing `m_dynamic_cols_`.
  //
  // Caller doesn't need to remember a follow-up `record_dynamic_col`
  // — historically that was a hidden requirement that silently
  // dropped post-init cols on the first reconstruct (juan/cascade
  // SIGSEGV).
  // 2026-05-11 fix — record unconditionally (no mode gate).  Under
  // `off`, `system_lp.cpp:560` still saves a snapshot, so the
  // aperture-path `clone_from_flat` consumes this buffer.  Without
  // recording α cols here, `apply_post_load_replay` re-installs cuts
  // that reference an α column index past the snapshot's numcols —
  // silently corrupt at best, segfault at worst (juan/IPLP 2026-05-11).
  // Throwaway clones (SDDP aperture chunk LP) skip recording — their
  // replay buffer is never re-applied to any backend.
  if (!m_replay_.replaying() && !m_is_throwaway_clone_
      && m_snapshot_holder_.has_data())
  {
    m_replay_.dynamic_cols_mut().push_back(col);
  }

  return col_idx;
}

ColIndex LinearInterface::add_col_raw(const SparseCol& col)
{
  // Raw path: `m_col_scales_` stays frozen so it can be shared via
  // shared_ptr across aperture clones.  `col.scale` is intentionally
  // ignored here — callers either pre-apply any scaling to `col.cost`
  // (replay path) or are passing back a SparseCol from a previous
  // `add_col(...)` whose scale is already recorded in the persistent
  // `m_col_scales_` snapshot from `load_flat`.  Either way, this path
  // does not (and must not) re-grow the scale vector.
  const auto col_idx = emit_col_to_backend(col);
  track_col_label_meta(col_idx, col);
  return col_idx;
}

ColIndex LinearInterface::add_col_disposable(const SparseCol& col)
{
  // Direct path to the solver backend — deliberately does NOT route
  // through `emit_col_to_backend`, `add_col`, or `add_col_raw` so the
  // disposable mutation surface stays trivially auditable
  // (`m_backend_`, `m_scale_objective_` read-only,
  // `m_post_clone_col_metas_*`, nothing else).  Bound normalisation
  // and the `scale_objective` divide are duplicated inline from
  // `emit_col_to_backend` (~5 lines) — deliberate maintenance cost
  // in exchange for strong containment of the shared-metadata
  // invariant on cloned LPs.
  ensure_backend();
  assert(col.scale == 1.0
         && "add_col_disposable: non-unit col.scale forbidden — "
            "shared m_col_scales_ must not grow on a clone");

  const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);
  const auto index = m_backend_->get_num_cols();
  m_backend_->add_col(lowb, uppb, col.cost / m_scale_objective_);
  invalidate_cached_optimal_on_mutation();
  const auto col_idx = ColIndex {index};

  // Capture the label-meta fields — mirrors `track_col_label_meta`
  // but writes to per-clone-local storage, NOT the shared metadata.
  const SparseColLabel meta {
      .class_name = col.class_name,
      .variable_name = col.variable_name,
      .variable_uid = col.variable_uid,
      .context = col.context,
  };

  // Eager dedup: check the per-instance post-flatten index (cuts,
  // alpha, cascade elastic) AND the per-clone disposable index.  Both
  // layers must reject duplicates so a disposable add cannot silently
  // shadow any earlier post-flatten or disposable entry.  Unlabelled
  // cols (empty class/variable + unknown_uid + monostate context) are
  // skipped, matching the production path.
  if (!is_empty_col_label(meta)) {
    if (auto pit = m_post_flatten_col_meta_index_.find(meta);
        pit != m_post_flatten_col_meta_index_.end())
    {
      throw std::runtime_error(std::format(
          "Duplicate disposable LP column metadata: class='{}' var='{}' "
          "uid={} (already present at post-flatten col {}, attempted "
          "disposable at col {})",
          meta.class_name,
          meta.variable_name,
          meta.variable_uid,
          pit->second,
          col_idx));
    }
    auto [it, inserted] =
        m_post_clone_col_meta_index_.try_emplace(meta, col_idx);
    if (!inserted) {
      throw std::runtime_error(std::format(
          "Duplicate disposable LP column metadata: class='{}' var='{}' "
          "uid={} (first disposable at col {}, duplicate at col {})",
          meta.class_name,
          meta.variable_name,
          meta.variable_uid,
          it->second,
          col_idx));
    }
  }
  m_post_clone_col_metas_.emplace_back(col_idx, meta);
  return col_idx;
}

RowIndex LinearInterface::add_row_disposable(const SparseRow& row,
                                             const double eps)
{
  // Direct path to the solver backend — see `add_col_disposable` for
  // the "self-contained mutation surface" rationale.  Deliberately
  // does NOT route through `emit_row_to_backend` / `add_row_raw`:
  // duplicating the ~2 lines of `to_flat` + low-level `add_row` is
  // the price we pay to guarantee that no future side-effect added
  // to the raw path can silently corrupt shared metadata on a
  // shallow clone.
  ensure_backend();
  assert(row.scale == 1.0
         && "add_row_disposable: non-unit row.scale forbidden — "
            "shared m_row_scales_ must not grow on a clone");

  // The low-level `add_row(name, ...)` helper applies
  // `normalize_bound` to the lb/ub itself (linear_interface.cpp
  // body), so we pass `row.lowb` / `row.uppb` verbatim.
  const auto [columns, elements] = row.to_flat<int>(eps);
  const std::string name;
  const auto row_idx =
      add_row(name, columns.size(), columns, elements, row.lowb, row.uppb);

  const SparseRowLabel meta {
      .class_name = row.class_name,
      .constraint_name = row.constraint_name,
      .variable_uid = row.variable_uid,
      .context = row.context,
  };

  if (!is_empty_row_label(meta)) {
    if (auto pit = m_post_flatten_row_meta_index_.find(meta);
        pit != m_post_flatten_row_meta_index_.end())
    {
      throw std::runtime_error(std::format(
          "Duplicate disposable LP row metadata: class='{}' cons='{}' "
          "uid={} (already present at post-flatten row {}, attempted "
          "disposable at row {})",
          meta.class_name,
          meta.constraint_name,
          meta.variable_uid,
          pit->second,
          row_idx));
    }
    auto [it, inserted] =
        m_post_clone_row_meta_index_.try_emplace(meta, row_idx);
    if (!inserted) {
      throw std::runtime_error(std::format(
          "Duplicate disposable LP row metadata: class='{}' cons='{}' "
          "uid={} (first disposable at row {}, duplicate at row {})",
          meta.class_name,
          meta.constraint_name,
          meta.variable_uid,
          it->second,
          row_idx));
    }
  }
  m_post_clone_row_metas_.emplace_back(row_idx, meta);
  return row_idx;
}

std::vector<ColIndex> LinearInterface::add_cols_disposable(
    std::span<const SparseCol> cols)
{
  // Bulk mirror of `add_col_disposable`.  No backend-level CSR fast
  // path exists for columns, so we simply iterate the singular
  // disposable insert and collect the returned indices.  All the
  // shared-metadata containment guarantees of the singular path
  // carry over verbatim because we route through it.
  std::vector<ColIndex> indices;
  indices.reserve(cols.size());
  for (const auto& col : cols) {
    indices.push_back(add_col_disposable(col));
  }
  return indices;
}

std::vector<RowIndex> LinearInterface::add_rows_disposable(
    std::span<const SparseRow> rows, const double eps)
{
  // Bulk mirror of `add_row_disposable`.  Same rationale as
  // `add_cols_disposable`: route through the singular path so the
  // per-row label-meta capture and disposable dedup stay confined
  // to the per-clone-local extras.
  std::vector<RowIndex> indices;
  indices.reserve(rows.size());
  for (const auto& row : rows) {
    indices.push_back(add_row_disposable(row, eps));
  }
  return indices;
}

ColIndex LinearInterface::emit_cols_to_backend(std::span<const SparseCol> cols,
                                               bool apply_col_scale)
{
  // Bulk equivalent of `emit_col_to_backend`: assembles CSC buffers
  // for the batch and dispatches a single `m_backend_->add_cols(...)`
  // call.  Shared by `add_cols(span)` (physical) and `add_cols_raw`
  // (raw) — both apply the same `cost / scale_objective` divisor and
  // `normalize_bounds` clipping.  Returns the index of the first
  // column added; callers compute per-element indices as
  // `first_col_index + c`.
  //
  // `SparseCol` carries no coefficient map, so each column's CSC
  // slice is empty (`colbeg[c+1] == colbeg[c]`).  The bulk-dispatch
  // value is amortising N per-column allocator hits in the backend's
  // internal column metadata array down to one.
  ensure_backend();
  const auto first_col_index = ColIndex {m_backend_->get_num_cols()};
  if (cols.empty()) {
    return first_col_index;
  }

  const auto ncols = static_cast<int>(std::ssize(cols));
  std::vector<int> colbeg(ncols + 1, 0);
  std::vector<int> colind;
  std::vector<double> colval;
  std::vector<double> collb(ncols);
  std::vector<double> colub(ncols);
  std::vector<double> colobj(ncols);

  const auto inv_so = 1.0 / m_scale_objective_;
  const bool validate = m_validation_options_.effective_enable();
  const double infy = m_backend_->infinity();
  const double infy_guard = infy * 0.999;
  auto col_idx = first_col_index;
  for (size_t c = 0; c < cols.size(); ++c, ++col_idx) {
    const auto& col = cols[c];
    const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);
    collb[c] = lowb;
    colub[c] = uppb;
    colobj[c] =
        apply_col_scale ? col.cost * col.scale * inv_so : col.cost * inv_so;
    colbeg[c + 1] = static_cast<int>(std::ssize(colind));

    // Validation hooks (Phase 2): note bounds + obj per column in
    // the bulk path so bulk callers see the same warnings as
    // singular `add_col(SparseCol)`.  Strong index → deferred format.
    if (validate) {
      if (std::abs(col.lowb) < infy_guard) {
        m_validation_stats_.note_bound(
            col.lowb, col_idx, m_validation_options_);
      }
      if (std::abs(col.uppb) < infy_guard) {
        m_validation_stats_.note_bound(
            col.uppb, col_idx, m_validation_options_);
      }
      m_validation_stats_.note_obj(col.cost, col_idx, m_validation_options_);
    }
  }

  m_backend_->add_cols(ncols,
                       colbeg.data(),
                       colind.data(),
                       colval.data(),
                       collb.data(),
                       colub.data(),
                       colobj.data());
  invalidate_cached_optimal_on_mutation();

  return first_col_index;
}

ColIndex LinearInterface::add_cols_raw(std::span<const SparseCol> cols)
{
  // Raw bulk path — mirrors `add_col_raw(SparseCol)`'s shape:
  // dispatch via the shared `emit_cols_to_backend` helper, then capture
  // label-meta.  `m_col_scales_` stays frozen so it can be shared
  // across aperture clones via `std::shared_ptr`.  `col.scale` is
  // intentionally ignored on every entry — see `add_col_raw` for the
  // rationale.
  const auto first_col = emit_cols_to_backend(cols, /*apply_col_scale=*/false);
  auto col_idx = first_col;
  for (const auto& col : cols) {
    track_col_label_meta(col_idx, col);
    ++col_idx;
  }
  return first_col;
}

ColIndex LinearInterface::add_cols(std::span<const SparseCol> cols)
{
  // Physical bulk path — mirrors `add_col(SparseCol)`'s shape:
  //   1. dispatch the bulk backend insert via `emit_cols_to_backend`
  //   2. register `col.scale` for any non-unity entries
  //   3. capture label-meta for every column
  //
  // Order matches the singular path: scale first, then label meta.
  const auto first_col = emit_cols_to_backend(cols, /*apply_col_scale=*/true);
  auto col_idx = first_col;
  for (const auto& col : cols) {
    if (col.scale != 1.0) {
      set_col_scale(col_idx, col.scale);
    }
    track_col_label_meta(col_idx, col);
    ++col_idx;
  }
  return first_col;
}

// ── Row operations ──

RowIndex LinearInterface::add_row(const std::string& name,
                                  const size_t numberElements,
                                  const std::span<const int>& columns,
                                  const std::span<const double>& elements,
                                  const double rowlb,
                                  const double rowub)
{
  const auto index = m_backend_->get_num_rows();
  const auto row_idx = RowIndex {index};

  m_backend_->add_row(static_cast<int>(numberElements),
                      columns.data(),
                      elements.data(),
                      normalize_bound(rowlb),
                      normalize_bound(rowub));
  invalidate_cached_optimal_on_mutation();

  // Uniqueness is enforced on metadata (via the build-time
  // LinearProblem dedup and the post-flatten dedup index) in
  // track_row_label_meta.  The string path here just records the
  // pre-formatted name when one is provided.
  if (m_label_maker_.row_names_enabled() && !name.empty()) {
    auto& rin = detach_for_write(m_row_index_to_name_);
    if (std::ssize(rin) <= index) {
      rin.resize(static_cast<size_t>(index) + 1);
    }
    rin[row_idx] = name;
  }

  return row_idx;
}

void LinearInterface::track_row_label_meta(RowIndex row_idx,
                                           const SparseRow& row)
{
  // Post-flatten path: see `track_col_label_meta` for full rationale.
  // Frozen `m_row_labels_meta_` is left untouched here; per-instance
  // `m_post_flatten_row_labels_meta_` absorbs every cut row, cascade
  // elastic constraint, etc.  No `ensure_labels_meta_decompressed()`
  // round-trip — the frozen side stays compressed unless `write_lp`
  // explicitly needs strings.
  const auto i = static_cast<size_t>(row_idx);
  const auto frozen_count = flatten_row_count();
  if (i < frozen_count) {
    return;  // frozen flatten-side row — load_flat is the sole writer
  }
  const auto post_offset = i - frozen_count;
  if (m_post_flatten_row_labels_meta_.size() <= post_offset) {
    m_post_flatten_row_labels_meta_.resize(post_offset + 1);
  }
  m_post_flatten_row_labels_meta_[post_offset] = SparseRowLabel {
      .class_name = row.class_name,
      .constraint_name = row.constraint_name,
      .variable_uid = row.variable_uid,
      .context = row.context,
  };

  // Eager metadata-based duplicate detection — see the `add_col`
  // companion for rationale.  Consults the per-instance
  // `m_post_flatten_row_meta_index_` so duplicate post-flatten
  // insertions (cuts, alpha, cascade elastic) against any previously
  // inserted post-flatten entry on this instance are reported.  The
  // `try_emplace` is preserved on the replay path so subsequent
  // readers (`add_row_disposable`, `delete_rows` rebuild, `clone()`)
  // see a coherent dedup map.
  const auto& meta = m_post_flatten_row_labels_meta_[post_offset];
  if (!is_empty_row_label(meta)) {
    auto [it, inserted] =
        m_post_flatten_row_meta_index_.try_emplace(meta, row_idx);
    if (!inserted) {
      throw std::runtime_error(
          std::format("Duplicate LP row metadata: class='{}' cons='{}' "
                      "uid={} (first at row {}, duplicate at row {})",
                      meta.class_name,
                      meta.constraint_name,
                      meta.variable_uid,
                      it->second,
                      row_idx));
    }
  }
}

RowIndex LinearInterface::add_row(const SparseRow& row, const double eps)
{
  ensure_backend();

  // Validation hooks (Phase 2): note each cmap coefficient + finite
  // RHS, and detect coefficients that the caller's `eps` will silently
  // drop.  Runs once per row at the public entry point so both the
  // compose_physical branch and the add_row_raw fallback see the same
  // accounting.  Costs one extra cmap pass per row when enabled.
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    const auto ridx = RowIndex {get_numrows()};
    for (const auto& [col, val] : row.cmap) {
      if (eps > 0.0 && std::abs(val) > 0.0 && std::abs(val) < eps
          && std::abs(val) >= 1e-30)
      {
        m_validation_stats_.note_filtered(
            val, eps, ridx, m_validation_options_);
      } else {
        m_validation_stats_.note_coeff(val, ridx, m_validation_options_);
      }
    }
    if (std::abs(row.lowb) < infy_guard) {
      m_validation_stats_.note_rhs(row.lowb, ridx, m_validation_options_);
    }
    if (std::abs(row.uppb) < infy_guard) {
      m_validation_stats_.note_rhs(row.uppb, ridx, m_validation_options_);
    }
  }

  // Decide whether to treat `row` as physical (apply col_scale + per-
  // row row-max equilibration) or as LP-space (pass through unchanged
  // apart from SparseRow::scale composition).
  //
  //  - m_base_numrows_set_ == false  → structural-build phase, i.e.
  //                                     add_row called from load_flat
  //                                     or from a caller building the
  //                                     initial matrix.  The bulk
  //                                     equilibration pass already
  //                                     normalised those rows, so a
  //                                     second pass would double-scale.
  //  - m_equilibration_method_ == none && m_col_scales_ empty
  //                                  → nothing to compose; physical ==
  //                                     LP in that case.
  //  - Otherwise                     → cut-phase physical row; apply
  //                                     col_scale + row-max.
  const bool is_cut_phase = m_base_numrows_set_;
  const bool have_col_scales = !m_col_scales_->empty();
  const bool have_equilibration =
      m_equilibration_method_ != LpEquilibrationMethod::none;
  // Objective-coupled cut rows (α-bearing SDDP / boundary cuts) must still
  // receive compose_physical's `÷ scale_objective` (step 1b) even when there
  // are no column scales and equilibration is off.  The objective itself was
  // divided by `scale_objective` at flatten(); a cut emitted raw would leave
  // α (future-cost $) AND the cut RHS under-weighted by `scale_objective`
  // relative to the rest of the objective, which inflates the reservoir
  // water-value duals and forces water hoarding.  `physical == LP` only holds
  // when `scale_objective == 1` too — so include it in the gate.
  const bool have_scale_obj = m_scale_objective_ != 1.0;
  const bool compose_physical =
      is_cut_phase && (have_col_scales || have_equilibration || have_scale_obj);

  if (!compose_physical) {
    return add_row_raw(row, eps);
  }

  // Physical-space cut insertion — operates directly on the flat
  // (columns, elements) representation to avoid building an
  // intermediate `SparseRow` and to keep the scale composition in one
  // place instead of bouncing through `add_row_raw` (which would
  // otherwise re-apply `SparseRow::scale` on top of our already-
  // composed divisor).
  auto [columns, elements] = row.to_flat<int>(eps);
  double lb = row.lowb;
  double ub = row.uppb;
  const auto infy = m_backend_->infinity();

  // 1. Physical → LP column scaling.  Columns beyond the stored
  //    `m_col_scales_` extent default to scale 1.0 (matching
  //    `get_col_scale`).
  const auto& col_scales_ref = *m_col_scales_;
  const auto n_col_scales = std::ssize(col_scales_ref);
  for (std::size_t k = 0; k < elements.size(); ++k) {
    const auto col = ColIndex {columns[k]};
    if (col < n_col_scales) {
      elements[k] *= col_scales_ref[col];
    }
  }

  // 2. SparseRow::scale composition (divides row contents by row.scale,
  //    same contract as `flatten()` applies to structural rows before
  //    the bulk equilibration pass).  We start `composite_scale` here
  //    and accumulate the scale_objective divisor in step 1b BEFORE the
  //    per-row equilibration in step 3.  Including scale_obj in
  //    `composite_scale` (rather than just scaling elements / bounds
  //    in place without recording it) is what makes the round-trip
  //    invariants hold: get_row_low()[cut] = raw_lb × composite_scale =
  //    phys_lb (verbatim), and get_row_dual()[cut] = raw_dual ×
  //    scale_obj / composite_scale = raw_dual × scale_obj / (scale_obj ×
  //    row_scale_part) = raw_dual / row_scale_part = π_phys (correct
  //    physical dual).
  double composite_scale = row.scale;
  if (row.scale != 1.0) {
    const auto inv_rs = 1.0 / row.scale;
    for (auto& v : elements) {
      v *= inv_rs;
    }
    if (lb > -infy && lb < infy) {
      lb *= inv_rs;
    }
    if (ub > -infy && ub < infy) {
      ub *= inv_rs;
    }
  }

  // 1b. Global objective scaling — folded into `composite_scale`.
  //
  //  SDDP optimality / feasibility cuts are objective-related rows
  //  (they bound α, which represents future cost in $-units).  The
  //  OBJ itself is divided by `scale_objective` at `flatten()` time
  //  (linear_problem.cpp:725-729); cut rows added later via this path
  //  were previously left un-divided, leaving cuts and obj in
  //  inconsistent scales by exactly `scale_objective` (juan/gtopt_iplp
  //  LMAULE: 4.2e+5 → 4.2e+8 → 4.2e+11 across consecutive backward
  //  phases at scale_obj=1000).
  //
  //  We divide elements + finite bounds by scale_objective AND record
  //  the divisor in `composite_scale` (which is later passed to
  //  `set_row_scale` so accessor views can recover the original
  //  physical bound / dual):
  //
  //    raw_lb_stored          = phys_lb / (scale_obj × row_scale_part)
  //    composite_scale_stored = scale_obj × row_scale_part
  //    get_row_low[i]         = raw_lb × composite_scale = phys_lb ✓
  //    get_row_dual[i]        = raw_dual × scale_obj / composite_scale
  //                           = raw_dual / row_scale_part = π_phys ✓
  //
  //  The previous form of this fix divided in-place WITHOUT folding
  //  into composite_scale; the resulting row stored `raw_lb = phys_lb /
  //  scale_obj` but `row_scale = 1`, so `get_row_low` returned
  //  `phys_lb / scale_obj` instead of `phys_lb` — a silent off-by-
  //  scale_obj that broke the physical readback contract.
  if (m_scale_objective_ != 1.0) {
    const auto inv_so = 1.0 / m_scale_objective_;
    for (auto& v : elements) {
      v *= inv_so;
    }
    if (lb > -infy && lb < infy) {
      lb *= inv_so;
    }
    if (ub > -infy && ub < infy) {
      ub *= inv_so;
    }
    composite_scale *= m_scale_objective_;
  }

  // 3. Per-row equilibration, only when the LP was built with
  //    equilibration on.  When the build chose `none` but col_scales
  //    are non-trivial (semantic scales set in flatten()), column
  //    scaling still runs at step 1; the row-max pass is skipped to
  //    match the historical invariant for non-equilibrated LPs.
  //
  //  Two normalization choices:
  //   - PIVOT-COLUMN normalization (when `row.pivot_col` is set):
  //     divide by elements[pivot_col].  Used for SDDP optimality
  //     cuts so that α's LP-space coefficient stays at 1.0 and
  //     state-link coefs absorb the dynamic range (which is then
  //     handled by the basis structure rather than being amplified
  //     into κ when α enters the basis).  Without this, row-max
  //     normalization would pick a state link as the divisor,
  //     pushing α down to O(10⁻⁹) on juan-scale problems and
  //     contributing κ ≈ 10⁹ per cut.
  //   - ROW-MAX normalization (default): divide by max-abs.  Right
  //     for structural rows where no single column carries the
  //     row's defining magnitude.
  if (have_equilibration) {
    double norm = 0.0;
    if (row.pivot_col != unknown_index) {
      // Find pivot_col's coefficient in the (already-step-1-and-1b-
      // composed) elements.  If not present (e.g. dropped by `eps`
      // filter in `to_flat`), fall back to row-max.
      for (std::size_t k = 0; k < elements.size(); ++k) {
        if (ColIndex {columns[k]} == row.pivot_col) {
          norm = std::abs(elements[k]);
          break;
        }
      }
    }
    if (norm == 0.0) {
      double max_abs = 0.0;
      for (const auto v : elements) {
        max_abs = std::max(max_abs, std::abs(v));
      }
      norm = max_abs;
    }
    if (norm > 0.0 && norm != 1.0) {
      const double inv = 1.0 / norm;
      for (auto& v : elements) {
        v *= inv;
      }
      if (lb > -infy && lb < infy) {
        lb *= inv;
      }
      if (ub > -infy && ub < infy) {
        ub *= inv;
      }
      composite_scale *= norm;
    }
  }

  // Lazy label: pass an empty name to the base `add_row`; the real
  // label is synthesised on demand by `generate_labels_from_maps`
  // and cached in `m_row_index_to_name_` at `write_lp` time.
  const auto row_idx =
      add_row(std::string {}, columns.size(), columns, elements, lb, ub);
  if (composite_scale != 1.0) {
    set_row_scale(row_idx, composite_scale);
  }
  track_row_label_meta(row_idx, row);
  // No auto-record for rows: the single-arg `add_row(SparseRow)` is
  // used both for structural rows (cascade elastic targets, but those
  // moved to bulk `add_rows` in 9fe35927) AND for cut rows (loaded
  // singly in tests, in bulk via SDDP cut loaders).  Cut callers use
  // `record_cut_row` to land the row in `m_active_cuts_` for the
  // separate cut-replay path; auto-recording into `m_dynamic_rows_`
  // here would double-record every cut on reconstruct.  Auto-record
  // is therefore restricted to `add_col(SparseCol)` where the
  // structural-vs-cut ambiguity does not arise (cuts add rows, not
  // cols).  Structural-row callers continue to use
  // `record_dynamic_row` explicitly.
  return row_idx;
}

RowIndex LinearInterface::emit_row_to_backend(const SparseRow& row,
                                              const double eps)
{
  // Pure backend emit — composes `SparseRow::scale` (mirroring
  // `flatten()`'s treatment of static rows) but applies no per-column
  // `col_scale` multiplication and no per-row equilibration.  Lazy
  // label: pass an empty name to the base `add_row`; the real label
  // is synthesised on demand by `generate_labels_from_maps`.
  ensure_backend();
  const std::string name;

  const auto rs = row.scale;

  if (rs != 1.0) {
    const auto inv_rs = 1.0 / rs;
    const auto infy = m_backend_->infinity();

    auto [columns, elements] = row.to_flat<int>(eps);
    for (auto& v : elements) {
      v *= inv_rs;
    }

    const double lb =
        (row.lowb > -infy && row.lowb < infy) ? row.lowb * inv_rs : row.lowb;
    const double ub =
        (row.uppb > -infy && row.uppb < infy) ? row.uppb * inv_rs : row.uppb;

    return add_row(name, columns.size(), columns, elements, lb, ub);
  }

  const auto [columns, elements] = row.to_flat<int>(eps);
  return add_row(name, columns.size(), columns, elements, row.lowb, row.uppb);
}

RowIndex LinearInterface::add_row_raw(const SparseRow& row, const double eps)
{
  // Internal raw-insertion path — called by the public `add_row` after
  // it has (optionally) composed col_scales and row-max equilibration
  // for physical-space cuts, or directly when the caller flagged the
  // row as already being in LP space.  No further per-column or per-
  // row equilibration happens here; we only compose `SparseRow::scale`
  // (the caller-specified row scaler, mirroring how `flatten()` handles
  // static rows).
  const auto row_idx = emit_row_to_backend(row, eps);
  if (row.scale != 1.0) {
    set_row_scale(row_idx, row.scale);
  }
  track_row_label_meta(row_idx, row);
  return row_idx;
}

void LinearInterface::add_rows(const std::span<const SparseRow> rows,
                               const double eps)
{
  ensure_backend();
  if (rows.empty()) {
    return;
  }

  // Dispatch through the same `compose_physical` gate as the singular
  // `add_row` so bulk and per-row callers see the same physical ↔ LP
  // semantics for batches of post-flatten cut rows.  The gate state
  // (`m_base_numrows_set_`, `m_col_scales_`, `m_equilibration_method_`)
  // is invariant across the batch, so it is checked once and applied
  // uniformly to every row.
  const bool is_cut_phase = m_base_numrows_set_;
  const bool have_col_scales = !m_col_scales_->empty();
  const bool have_equilibration =
      m_equilibration_method_ != LpEquilibrationMethod::none;
  // See the singular `add_row` gate: objective-coupled cut rows need the
  // `÷ scale_objective` composition even with no col_scales / equilibration,
  // so `physical == LP` only when `scale_objective == 1` too.
  const bool have_scale_obj = m_scale_objective_ != 1.0;
  const bool compose_physical =
      is_cut_phase && (have_col_scales || have_equilibration || have_scale_obj);

  if (!compose_physical) {
    add_rows_raw(rows, eps);
    return;
  }

  // Compose every row's physical → LP transform once across the batch
  // and delegate to `add_rows_raw` with already-composed values.  This
  // collapses N CPXaddrows calls into one and N×3 element walks into
  // N×1 (the previous fallback was per-row `add_row`, which dominated
  // the cut-replay cost under `LowMemoryMode::compress` — see the
  // 2026-05 LB-overshoot investigation in support/juan/).
  //
  // Composition contract (matches the singular `add_row(SparseRow)`
  // compose_physical body at :1929-2071):
  //
  //   composed.cmap[col]  = phys[col] × col_scale[col]
  //   composed.scale      = row.scale × scale_objective × equil_norm
  //   composed.lowb/.uppb = phys (unchanged; `add_rows_raw` divides
  //                         finite bounds by `composed.scale`)
  //   composed.pivot_col, labels, context: copied from `row`
  //
  // `add_rows_raw` then divides cmap and finite bounds by
  // `composed.scale` and stores `composed.scale` via `set_row_scale`,
  // so accessor views (`get_row_low`, `get_row_dual`) recover physical
  // values verbatim — same invariant as the singular path:
  //
  //   raw_lb_stored          = phys_lb / composite_scale
  //   composite_scale_stored = composite_scale
  //   get_row_low[i]         = raw_lb × composite_scale = phys_lb ✓
  //   get_row_dual[i]        = raw_dual × scale_obj / composite_scale
  //                          = π_phys ✓
  std::vector<SparseRow> composed;
  composed.reserve(rows.size());

  const auto& col_scales_ref = *m_col_scales_;
  const auto n_col_scales = std::ssize(col_scales_ref);

  for (const auto& row : rows) {
    SparseRow c {
        .lowb = row.lowb,
        .uppb = row.uppb,
        .cmap = {},
        .scale = row.scale,
        .pivot_col = row.pivot_col,
        .class_name = row.class_name,
        .constraint_name = row.constraint_name,
        .variable_uid = row.variable_uid,
        .context = row.context,
    };
    c.reserve(row.cmap.size());

    // 1. Apply per-column physical → LP scaling to cmap values while
    //    cloning the input row's coefficient map.  Built from scratch
    //    rather than copy + in-place mutate so the loop also works
    //    against `std::flat_map`'s proxy iterator (which yields
    //    temporaries that cannot bind to a non-const reference).
    if (have_col_scales) {
      for (const auto& kv : row.cmap) {
        const auto col = kv.first;
        double val = kv.second;
        if (col < n_col_scales) {
          val *= col_scales_ref[col];
        }
        c.cmap.emplace(col, val);
      }
    } else {
      c.cmap = row.cmap;
    }

    // 2. Per-row equilibration norm — computed from post-col-scale,
    //    post-row.scale, post-scale_obj values to match the singular
    //    path's step-3 normalisation (linear_interface.cpp:2038-2071).
    //    Pivot-column normalisation when set; row-max otherwise.
    double composite_scale = row.scale * m_scale_objective_;
    if (have_equilibration) {
      const double inv_rs_so = 1.0 / composite_scale;
      double norm = 0.0;
      if (row.pivot_col != unknown_index) {
        if (auto it = c.cmap.find(row.pivot_col); it != c.cmap.end()) {
          norm = std::abs(it->second * inv_rs_so);
        }
      }
      if (norm == 0.0) {
        for (const auto& kv : c.cmap) {
          norm = std::max(norm, std::abs(kv.second * inv_rs_so));
        }
      }
      if (norm > 0.0 && norm != 1.0) {
        composite_scale *= norm;
      }
    }

    c.scale = composite_scale;
    composed.push_back(std::move(c));
  }

  add_rows_raw(composed, eps);
}

void LinearInterface::add_rows_raw(const std::span<const SparseRow> rows,
                                   const double eps)
{
  // Bulk LP-raw insertion path — companion to `add_row_raw`.  Skips
  // col_scale × elem, scale_objective divisor, and per-row row-max
  // equilibration; only `SparseRow::scale` is composed (mirroring how
  // `flatten()` handles static rows).  Use this when the caller already
  // has LP-raw coefficients/bounds and wants the bulk speedup without
  // compose_physical.
  ensure_backend();
  if (rows.empty()) {
    return;
  }

  const auto nrows = static_cast<int>(std::ssize(rows));
  const auto first_row_index = RowIndex {m_backend_->get_num_rows()};

  // Sum cmap sizes for the CSR reserve hint.  When `eps > 0` this
  // over-reserves slightly (rows whose abs(val) ≤ eps are dropped at
  // fill time), but `reserve()` is a hint and the over-allocation is
  // harmless — strictly cheaper than the prior count-pass which walked
  // every cmap value once just for sizing.
  size_t total_nnz = 0;
  for (const auto& row : rows) {
    total_nnz += row.cmap.size();
  }

  // Allocate CSR arrays
  std::vector<int> rowbeg(nrows + 1);
  std::vector<int> rowind;
  std::vector<double> rowval;
  std::vector<double> rowlb(nrows);
  std::vector<double> rowub(nrows);
  rowind.reserve(total_nnz);
  rowval.reserve(total_nnz);

  const auto infy = m_backend_->infinity();
  const double infy_guard = infy * 0.999;
  // Skip per-element validation hooks during a cut-replay reconstruct.
  // The cuts in `m_active_cuts_` were already validated when first
  // inserted, and replay re-injects the SAME values back into the
  // backend after a `release_backend()` → `reconstruct_backend()`
  // cycle.  Re-running `note_coeff` / `note_filtered` per-element on
  // every replay dominates the cut-replay cost under
  // `LowMemoryMode::compress` (1.2M coefs/cell × ~1670 cells × ~10ns/
  // call ≈ 20s/iter at high cut counts on Juan/IPLP).
  const bool validate =
      !m_replay_.replaying() && m_validation_options_.effective_enable();

  // Second pass: fill CSR arrays with scaled coefficients and bounds.
  auto row_idx = first_row_index;
  for (const auto& [r, row] : enumerate(rows)) {
    rowbeg[r] = static_cast<int>(rowind.size());

    const auto rs = row.scale;
    const auto inv_rs = (rs != 1.0) ? 1.0 / rs : 1.0;

    for (const auto& [col, val] : row.cmap) {
      const auto scaled = val * inv_rs;
      if (std::abs(scaled) > eps) {
        rowind.push_back(col);
        rowval.push_back(scaled);
        if (validate) {
          m_validation_stats_.note_coeff(
              scaled, row_idx, m_validation_options_);
        }
      } else if (validate && eps > 0.0 && std::abs(scaled) > 0.0
                 && std::abs(scaled) >= 1e-30)
      {
        m_validation_stats_.note_filtered(
            scaled, eps, row_idx, m_validation_options_);
      }
    }

    // Scale and normalize bounds
    auto lb = row.lowb;
    auto ub = row.uppb;
    if (rs != 1.0) {
      if (lb > -infy && lb < infy) {
        lb *= inv_rs;
      }
      if (ub > -infy && ub < infy) {
        ub *= inv_rs;
      }
    }
    rowlb[r] = normalize_bound(lb);
    rowub[r] = normalize_bound(ub);
    if (validate) {
      if (std::abs(lb) < infy_guard) {
        m_validation_stats_.note_rhs(lb, row_idx, m_validation_options_);
      }
      if (std::abs(ub) < infy_guard) {
        m_validation_stats_.note_rhs(ub, row_idx, m_validation_options_);
      }
    }
    ++row_idx;
  }
  rowbeg[nrows] = static_cast<int>(rowind.size());

  // Dispatch bulk add to solver backend
  m_backend_->add_rows(nrows,
                       rowbeg.data(),
                       rowind.data(),
                       rowval.data(),
                       rowlb.data(),
                       rowub.data());
  invalidate_cached_optimal_on_mutation();

  // Update row scales, label metadata, and name maps for all new rows.
  // `track_row_label_meta` is what keeps `m_row_labels_meta_` in sync with
  // the backend row count — without it, a later `generate_labels_from_maps`
  // call (e.g. through `write_lp` / `push_names_to_solver`) throws
  // "row N has no entry in m_row_labels_meta_".  This bulk path is hit
  // from `apply_post_load_replay` when a low-memory cell rebuilds and
  // re-injects `m_active_cuts_`; the single-row `add_row_raw` path
  // already calls `track_row_label_meta`, so mirroring it here keeps the
  // size invariant across compress/replay cycles.
  //
  // Bulk-grow `m_post_flatten_row_labels_meta_` and reserve the dedup
  // map up front: per-cut growth would resize-by-1 N times and rehash
  // the unordered_map several times across the batch, which dominates
  // the `track_row_label_meta` cost on the cut-replay hot path.
  {
    const auto frozen_count = flatten_row_count();
    const auto first_post_offset =
        std::cmp_greater_equal(static_cast<Index>(first_row_index),
                               frozen_count)
        ? static_cast<size_t>(first_row_index) - frozen_count
        : 0U;
    const auto needed_size = first_post_offset + static_cast<size_t>(nrows);
    if (m_post_flatten_row_labels_meta_.size() < needed_size) {
      m_post_flatten_row_labels_meta_.resize(needed_size);
    }
    m_post_flatten_row_meta_index_.reserve(m_post_flatten_row_meta_index_.size()
                                           + static_cast<size_t>(nrows));
  }

  auto bookkeep_idx = first_row_index;
  for (const auto& row : rows) {
    if (row.scale != 1.0) {
      set_row_scale(bookkeep_idx, row.scale);
    }

    track_row_label_meta(bookkeep_idx, row);

    if (m_label_maker_.row_names_enabled()) {
      const auto name = m_label_maker_.make_row_label(row);
      if (!name.empty()) {
        const auto i = static_cast<size_t>(bookkeep_idx);
        auto& rin = detach_for_write(m_row_index_to_name_);
        if (rin.size() <= i) {
          rin.resize(i + 1);
        }
        rin[bookkeep_idx] = name;
      }
    }
    ++bookkeep_idx;
  }
}

void LinearInterface::delete_rows(const std::span<const int> indices)
{
  ensure_backend();
  if (indices.empty()) {
    return;
  }

  m_backend_->delete_rows(static_cast<int>(indices.size()), indices.data());

  // Erase the deleted rows from both the formatted-name cache and
  // the label-metadata vector.  Iterate in reverse so positional
  // indices stay valid as we shrink each container.
  //
  // Frozen invariant: `m_row_labels_meta_` (set by `load_flat`) is
  // never erased — only post-flatten rows (cuts, cascade elastic
  // constraints, …) are subject to `delete_rows`.  In production the
  // SDDP cut store only deletes indices `>= base_numrows()` which
  // exactly matches `flatten_row_count()`, so any frozen-side
  // deletion would be a programming error.  Defensive guard below
  // skips frozen-side deletions silently rather than asserting, so
  // `row_label_at(idx)` semantics remain consistent if the caller
  // somehow passes a frozen index.
  auto& rin = detach_for_write(m_row_index_to_name_);
  const auto frozen_count = flatten_row_count();
  for (const auto idx : indices | std::views::reverse) {
    const auto pos = static_cast<ptrdiff_t>(idx);
    if (pos < std::ssize(rin)) {
      rin.erase(rin.begin() + pos);
    }
    if (std::cmp_less(idx, frozen_count)) {
      continue;
    }
    const auto post_pos = static_cast<std::size_t>(idx) - frozen_count;
    if (post_pos < m_post_flatten_row_labels_meta_.size()) {
      m_post_flatten_row_labels_meta_.erase(
          m_post_flatten_row_labels_meta_.begin()
          + static_cast<ptrdiff_t>(post_pos));
    }
  }
  rebuild_row_name_maps();

  // Row indices shifted on deletion, so every previously-registered
  // `(metadata → RowIndex)` pair on the post-flatten side is now
  // potentially stale.  Rebuild from the current
  // `m_post_flatten_row_labels_meta_`.
  m_post_flatten_row_meta_index_.clear();
  m_post_flatten_row_meta_index_.reserve(
      m_post_flatten_row_labels_meta_.size());
  for (const auto [i, label] : enumerate(m_post_flatten_row_labels_meta_)) {
    if (!label.class_name.empty() || !label.constraint_name.empty()
        || label.variable_uid != unknown_uid
        || !std::holds_alternative<std::monostate>(label.context))
    {
      const auto global_idx = RowIndex {static_cast<Index>(frozen_count + i)};
      m_post_flatten_row_meta_index_.emplace(label, global_idx);
    }
  }
}

void LinearInterface::rebuild_row_name_maps()
{
  if (m_label_maker_.duplicates_are_errors()) {
    auto& rn = detach_for_write(m_row_names_);
    const auto& rin = *m_row_index_to_name_;
    rn.clear();
    rn.reserve(rin.size());
    for (const auto [i, name] : enumerate<RowIndex>(rin)) {
      if (!name.empty()) {
        rn.emplace(name, i);
      }
    }
  }
}

void LinearInterface::reset_from(const LinearInterface& source,
                                 const size_t base_rows)
{
  const auto total_rows = static_cast<size_t>(m_backend_->get_num_rows());
  if (total_rows > base_rows) {
    const auto n_to_delete = total_rows - base_rows;
    std::vector<int> indices;
    indices.reserve(n_to_delete);
    for (auto i = base_rows; i < total_rows; ++i) {
      indices.push_back(static_cast<int>(i));
    }
    m_backend_->delete_rows(static_cast<int>(n_to_delete), indices.data());
  }

  const auto ncols = m_backend_->get_num_cols();
  const auto src_col_lo = source.get_col_low_raw();
  const auto src_col_hi = source.get_col_upp_raw();
  for (const auto [c, lo] :
       enumerate<ColIndex>(src_col_lo | std::views::take(ncols)))
  {
    m_backend_->set_col_lower(c, lo);
    m_backend_->set_col_upper(c, src_col_hi[c]);
  }

  const auto nrows = m_backend_->get_num_rows();
  const auto src_row_lo = source.get_row_low_raw();
  const auto src_row_hi = source.get_row_upp_raw();
  for (const auto [r, lo] :
       enumerate<RowIndex>(src_row_lo | std::views::take(nrows)))
  {
    m_backend_->set_row_lower(r, lo);
    m_backend_->set_row_upper(r, src_row_hi[r]);
  }

  if (m_label_maker_.row_names_enabled()) {
    detach_for_write(m_row_index_to_name_).resize(static_cast<size_t>(nrows));
    rebuild_row_name_maps();
  }
  invalidate_cached_optimal_on_mutation();
}

// ── Coefficients ──

// ── Raw coefficient accessors (LP units) ──

void LinearInterface::set_coeff_raw(const RowIndex row,
                                    const ColIndex column,
                                    const double value)
{
  ensure_backend();
  if (m_validation_options_.effective_enable()) {
    m_validation_stats_.note_coeff(value, row, column, m_validation_options_);
  }
  m_backend_->set_coeff(row, column, value);
  invalidate_cached_optimal_on_mutation();
  // Record post-snapshot coefficient overrides so
  // `clone_from_flat(with_replay=true)` and `apply_post_load_replay`
  // re-apply them on top of the snapshot's construction-time matval.
  // Without this, SDDP aperture clones inherited construction-time
  // piecewise seepage / production-factor coefficients while the live
  // backend carried the segment-corrected values — producing apertures
  // that were structurally infeasible against the propagated forward
  // state (juan/gtopt_iplp p51 INFEAS-PROBE 2026-05-12).  Mirrors the
  // `set_col_bounds_raw` replay-record path.
  if (!m_replay_.replaying() && !m_is_throwaway_clone_) {
    m_replay_.set_pending_coeff(row, column, value);
  }
}

double LinearInterface::get_coeff_raw(const RowIndex row,
                                      const ColIndex column) const
{
  return m_backend_->get_coeff(row, column);
}

// ── Physical coefficient accessors ──
//
// Source-of-truth for the physical ↔ LP coefficient transform is
// `flatten()` in linear_problem.cpp:346-369:
//
//     physical_value = LP_value × col_scale
//   → LP_coeff       = phys_coeff × col_scale
//
// and after row-scale composition (linear_problem.cpp:369):
//
//     LP_coeff       = phys_coeff × col_scale / row_scale
//
// `add_row` (linear_interface.cpp:1265-1270 cut-phase compose_physical
// branch) multiplies elements by `col_scale_ref[col]` before insertion,
// which is the same direction.
//
// Asymmetry vs. bound setters (set_col_low/set_col_upp):
//
//   - Bound: `lowb_LP   = lowb_phys / col_scale`
//            (the bound applies to x_LP, and x_LP = x_phys / col_scale)
//   - Coeff: `coef_LP   = coef_phys × col_scale`
//            (compensates for x_LP = x_phys / col_scale so that the
//             constraint Σ a × x evaluated in physical units is the same
//             value evaluated in LP units)
//   - RHS:   `rhs_LP    = rhs_phys / row_scale`
//            (only row-scale; col_scale never enters the RHS)
//
// Prior to 2026-05 these helpers had the col_scale direction inverted,
// which left juan/gtopt_iplp cuts off by `col_scale × col_scale` per
// SDDP iteration and produced LB > UB after iter 1.

void LinearInterface::set_coeff(const RowIndex row,
                                const ColIndex column,
                                const double physical_value)
{
  const double cs = get_col_scale(column);
  const double rs = get_row_scale(row);
  set_coeff_raw(row, column, physical_value * cs / rs);
}

double LinearInterface::get_coeff(const RowIndex row,
                                  const ColIndex column) const
{
  const double raw = get_coeff_raw(row, column);
  const double cs = get_col_scale(column);
  const double rs = get_row_scale(row);
  return raw * rs / cs;
}

bool LinearInterface::supports_set_coeff() const noexcept
{
  return m_backend_->supports_set_coeff();
}

// ── Simple delegations ──

void LinearInterface::set_obj_coeff_raw(const ColIndex index,
                                        const double value)
{
  ensure_backend();
  if (m_validation_options_.effective_enable()) {
    m_validation_stats_.note_obj(value, index, m_validation_options_);
  }
  m_backend_->set_obj_coeff(index, value);
}

void LinearInterface::set_obj_coeff(const ColIndex index,
                                    const double physical_value)
{
  // Mirrors flatten()'s objective composition (linear_problem.cpp):
  //   objval[i] = col.cost * col_scale[i]   (physical → LP cost)
  //   objval[i] /= scale_objective          (global scale_objective divisor)
  // Using `get_col_scale(index)` matches the singular bound setter
  // pattern (`set_col_low`, `set_col_upp`) and degrades to 1.0 when
  // `m_col_scales_` is empty (bare LinearInterface / unflattened LP),
  // making this a pure pass-through in the unscaled case.
  const double cs = get_col_scale(index);
  set_obj_coeff_raw(index, physical_value * cs / m_scale_objective_);
}

void LinearInterface::set_obj_coeffs_raw(std::span<const double> values)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  // Validation hooks (Phase 2): bulk obj writes feed `note_obj` per
  // value with a synthesised ColIndex location, mirroring the
  // singular path's accounting.  The deferred-format overload only
  // formats the location on the cold (huge-coefficient) branch.
  if (m_validation_options_.effective_enable()) {
    auto cidx = ColIndex {0};
    for (const auto v : values) {
      m_validation_stats_.note_obj(v, cidx, m_validation_options_);
      ++cidx;
    }
  }
  m_backend_->set_obj_coeffs(values.data(), static_cast<int>(values.size()));
  invalidate_cached_optimal_on_mutation();
}

// ── Raw column bound setters (LP/solver units) ──

namespace
{
/// Validate a (possibly-stale) ColIndex against the live backend's
/// numcols and throw a descriptive exception if out of range.  The
/// historical failure mode was a segfault inside the solver plugin
/// (CPXchgbds, similar) when a cached col index referenced a column
/// that disappeared on a low-memory rebuild — converting that into a
/// clean ``std::out_of_range`` lets the worker future propagate the
/// error to ``run_*_pass_all_scenes`` and produces a useful exit
/// message instead of an opaque SIGSEGV.
[[gnu::noinline]]
void check_col_index(const char* fn, ColIndex index, std::int64_t ncols)
{
  if (static_cast<std::int64_t>(index) < 0
      || static_cast<std::int64_t>(index) >= ncols)
  {
    throw std::out_of_range(
        std::format("LinearInterface::{}: col index {} out of range "
                    "[0, {}) — likely a stale ColIndex captured before the "
                    "backend was released and rebuilt with a different layout.",
                    fn,
                    static_cast<std::int64_t>(index),
                    ncols));
  }
}
}  // namespace

// Per-element wrappers — all bookkeeping (validation, normalize_bound,
// replay-buffer pending-bound capture, cached-optimal invalidation) lives
// once in `set_col_bounds_raw` / `set_col_bounds`.  The 1-element bulk
// dispatch costs three tiny stack arrays and one extra function call;
// the dominant cost — the virtual backend mutation — is unchanged.

void LinearInterface::set_col_low_raw(const ColIndex index, const double value)
{
  const std::array<ColIndex, 1> idx {index};
  const std::array<char, 1> lu {'L'};
  const std::array<double, 1> vals {value};
  set_col_bounds_raw(idx, lu, vals);
}

void LinearInterface::set_col_upp_raw(const ColIndex index, const double value)
{
  const std::array<ColIndex, 1> idx {index};
  const std::array<char, 1> lu {'U'};
  const std::array<double, 1> vals {value};
  set_col_bounds_raw(idx, lu, vals);
}

void LinearInterface::set_col_raw(const ColIndex index, const double value)
{
  const std::array<ColIndex, 1> idx {index};
  const std::array<char, 1> lu {'B'};
  const std::array<double, 1> vals {value};
  set_col_bounds_raw(idx, lu, vals);
}

// ── Physical column bound setters (physical_value / col_scale → LP) ──
//
// Routes through `set_col_bounds` which handles the ±DblMax / ± solver
// infinity / ± std::inf passthrough before applying the col_scale
// descale.  Note: prior to the bulk-unification, the singular setters
// did `physical_value / get_col_scale(index)` unconditionally, which
// silently turned `±DblMax` (a semantic "unbounded" marker) into a
// huge-but-finite value that `normalize_bound` could no longer map
// back to the solver's signed infinity.  The new path fixes that.
//
// Performance note: the singular path deliberately avoids the
// snap-to-existing-bound logic that e58add8f introduced, because the
// aperture-backward pass calls these setters tens of thousands of
// times per iteration and the extra virtual dispatches caused a ~20x
// slowdown on Juan/gtopt_iplp.  The propagation-noise problem that
// motivated the snap is already handled on the *read* side by
// `get_col_sol()`'s optimal-only bound clamp.
void LinearInterface::set_col_low(const ColIndex index,
                                  const double physical_value)
{
  const std::array<ColIndex, 1> idx {index};
  const std::array<char, 1> lu {'L'};
  const std::array<double, 1> vals {physical_value};
  set_col_bounds(idx, lu, vals);
}

void LinearInterface::set_col_upp(const ColIndex index,
                                  const double physical_value)
{
  const std::array<ColIndex, 1> idx {index};
  const std::array<char, 1> lu {'U'};
  const std::array<double, 1> vals {physical_value};
  set_col_bounds(idx, lu, vals);
}

void LinearInterface::set_col(const ColIndex index, const double physical_value)
{
  const std::array<ColIndex, 1> idx {index};
  const std::array<char, 1> lu {'B'};
  const std::array<double, 1> vals {physical_value};
  set_col_bounds(idx, lu, vals);
}

// ── Bulk column-bound setters ──────────────────────────────────────────────
//
// Mirrors `set_obj_coeffs_raw(span)` for the column-bound axis.  The phys
// variant descales finite values by `col_scale[idx[i]]`, but passes
// `±DblMax` / `± solver infinity()` / `± std::numeric_limits<double>::
// infinity()` through unchanged so `normalize_bound` in the raw layer
// can still map them to the active solver's signed infinity.  Dividing
// a finite solver-infinity (e.g. HiGHS 1e30) by a non-unit scale would
// otherwise yield a huge-but-finite value that no longer rounds to the
// solver's infinity threshold, producing an incorrect tight bound.

void LinearInterface::set_col_bounds_raw(
    const std::span<const ColIndex> indices,
    const std::span<const char> lu,
    const std::span<const double> values)
{
  if (indices.empty()) {
    return;
  }
  if (indices.size() != lu.size() || indices.size() != values.size()) {
    throw std::invalid_argument(
        std::format("LinearInterface::set_col_bounds_raw: span size mismatch "
                    "(indices={}, lu={}, values={})",
                    indices.size(),
                    lu.size(),
                    values.size()));
  }

  ensure_backend();
  assert(m_backend_ != nullptr);

  const auto ncols = m_backend_->get_num_cols();
  const double inf = m_backend_->infinity();
  const double infy_guard = inf * 0.999;
  const bool validate = m_validation_options_.effective_enable();
  const bool record_replay = !m_replay_.replaying() && !m_is_throwaway_clone_;

  // Live raw bounds — needed to populate the OTHER side of a 'L'/'U'
  // pending entry when the column is seen for the first time.  Reading
  // a backend pointer is allocation-free and we only deref it under
  // `record_replay`, so the cost is negligible.
  const double* col_low_live =
      record_replay ? m_backend_->col_lower() : nullptr;
  const double* col_upp_live =
      record_replay ? m_backend_->col_upper() : nullptr;

  const auto n = indices.size();
  std::vector<int> bk_idx;
  std::vector<char> bk_lu;
  std::vector<double> bk_vals;
  bk_idx.reserve(n);
  bk_lu.reserve(n);
  bk_vals.reserve(n);

  for (std::size_t i = 0; i < n; ++i) {
    const ColIndex c = indices[i];
    check_col_index("set_col_bounds_raw", c, ncols);
    const char which = lu[i];
    const double raw = values[i];
    const double normalised = normalize_bound(raw);

    if (validate && std::abs(raw) < infy_guard) {
      m_validation_stats_.note_bound(raw, c, m_validation_options_);
    }

    bk_idx.push_back(c);
    bk_lu.push_back(which);
    bk_vals.push_back(normalised);

    if (record_replay) {
      const auto cu = static_cast<std::size_t>(c);
      switch (which) {
        case 'L': {
          const double upper_now = col_upp_live[cu];
          m_replay_.set_pending_col_lower(c, normalised, upper_now);
          break;
        }
        case 'U': {
          const double lower_now = col_low_live[cu];
          m_replay_.set_pending_col_upper(c, lower_now, normalised);
          break;
        }
        case 'B':
          m_replay_.set_pending_col_lower(c, normalised, normalised);
          m_replay_.set_pending_col_upper(c, normalised, normalised);
          break;
        default:
          // Same default-skip behaviour as the SolverBackend fallback.
          break;
      }
    }
  }

  m_backend_->set_col_bounds_bulk(static_cast<int>(bk_idx.size()),
                                  bk_idx.data(),
                                  bk_lu.data(),
                                  bk_vals.data());
  invalidate_cached_optimal_on_mutation();
}

void LinearInterface::set_col_bounds(
    const std::span<const ColIndex> indices,
    const std::span<const char> lu,
    const std::span<const double> physical_values)
{
  if (indices.empty()) {
    return;
  }
  if (indices.size() != lu.size() || indices.size() != physical_values.size()) {
    throw std::invalid_argument(
        std::format("LinearInterface::set_col_bounds: span size mismatch "
                    "(indices={}, lu={}, physical_values={})",
                    indices.size(),
                    lu.size(),
                    physical_values.size()));
  }

  // Cache `inf` once — `infinity()` walks a virtual dispatch into
  // the backend (or the cached fallback when the backend is released),
  // and we want one such call per bulk dispatch, not N.  Ensure the
  // backend is loaded first so we read the active solver's infinity,
  // not the stale `m_cached_infinity_` from before reconstruct.
  ensure_backend();
  const double inf = infinity();

  const auto n = physical_values.size();
  std::vector<double> raw_values;
  raw_values.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double phys = physical_values[i];
    // Pass through ±DblMax / ± solver infinity / ± std::inf without
    // applying col_scale.  `inf <= DblMax` is the universal solver
    // invariant (CPLEX 1e20, HiGHS 1e30, OSI/CLP 1e30, all ≤ DblMax
    // ≈ 1.8e308), so `phys >= inf` subsumes both `phys >= DblMax`
    // and `std::isinf(phys) && phys > 0`.  Checked explicitly against
    // both inf and DblMax so a future solver with `infinity() > DblMax`
    // would still be covered.  Symmetric check on the negative side
    // collapsed into the same branch — clang-tidy `bugprone-branch-clone`
    // flagged the prior split as a duplicate branch body.
    const bool is_unbounded =
        phys >= inf || phys >= DblMax || phys <= -inf || phys <= -DblMax;
    raw_values.push_back(is_unbounded ? phys
                                      : phys / get_col_scale(indices[i]));
  }
  set_col_bounds_raw(indices, lu, raw_values);
}

// ── Raw row bound setters (LP/solver units) ──

void LinearInterface::set_row_low_raw(const RowIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(value) < infy_guard) {
      m_validation_stats_.note_rhs(value, index, m_validation_options_);
    }
  }
  m_backend_->set_row_lower(index, normalize_bound(value));
  invalidate_cached_optimal_on_mutation();
}

void LinearInterface::set_row_upp_raw(const RowIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(value) < infy_guard) {
      m_validation_stats_.note_rhs(value, index, m_validation_options_);
    }
  }
  m_backend_->set_row_upper(index, normalize_bound(value));
  invalidate_cached_optimal_on_mutation();
}

void LinearInterface::set_row_bounds_raw(const RowIndex row,
                                         const double lowb,
                                         const double uppb)
{
  // Mirror the single-side ``set_row_low_raw`` / ``set_row_upp_raw``
  // entry contract: ensure the backend is live before delegating.
  // A mutation issued after ``release_backend()`` would null-deref
  // ``m_backend_`` (flagged by clang-analyzer-core.CallAndMessage).
  ensure_backend();
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(lowb) < infy_guard) {
      m_validation_stats_.note_rhs(lowb, row, m_validation_options_);
    }
    if (std::abs(uppb) < infy_guard && uppb != lowb) {
      m_validation_stats_.note_rhs(uppb, row, m_validation_options_);
    }
  }
  m_backend_->set_row_bounds(row, normalize_bound(lowb), normalize_bound(uppb));
  invalidate_cached_optimal_on_mutation();
  // Companion replay record — equality rows record into the legacy
  // ``pending_rhs`` channel so the Seepage / RDL-equality replay
  // path under ``LowMemoryMode::compress`` keeps working without
  // schema changes.  Non-equality bounds (``lowb != uppb``) are NOT
  // currently replayed; callers that need to survive a backend
  // reconstruct between sets should use the per-phase
  // ``update_lp_for_phase`` path which re-issues the writes from
  // its own state.  See ``set_coeff_raw`` for the wider rationale.
  if (!m_replay_.replaying() && !m_is_throwaway_clone_ && lowb == uppb) {
    m_replay_.set_pending_rhs(row, lowb);
  }
}

// ── Physical row bound setters (physical × row_scale → LP) ──

void LinearInterface::set_row_low(const RowIndex index,
                                  const double physical_value)
{
  const double scale = get_row_scale(index);
  set_row_low_raw(index, physical_value / scale);
}

void LinearInterface::set_row_upp(const RowIndex index,
                                  const double physical_value)
{
  const double scale = get_row_scale(index);
  set_row_upp_raw(index, physical_value / scale);
}

void LinearInterface::set_row_bounds(const RowIndex row,
                                     const double physical_lowb,
                                     const double physical_uppb)
{
  const double scale = get_row_scale(row);
  set_row_bounds_raw(row, physical_lowb / scale, physical_uppb / scale);
}

void LinearInterface::set_row_equal_to_raw(const RowIndex row,
                                           const double value)
{
  // Convenience: equality is just both bounds at the same value.
  // Replaces the legacy ``set_rhs_raw`` name — see the header for
  // why the old name was removed.
  set_row_bounds_raw(row, value, value);
}

void LinearInterface::set_row_equal_to(const RowIndex row,
                                       const double physical_value)
{
  const double scale = get_row_scale(row);
  set_row_equal_to_raw(row, physical_value / scale);
}

Index LinearInterface::get_numrows() const
{
  if (m_backend_released_) {
    return m_cache_.numrows();
  }
  return m_backend_->get_num_rows();
}

Index LinearInterface::get_numcols() const
{
  // Defensive against the async-iter-overlap race: a worker thread can
  // call `release_backend()` on a cell whose state the main thread is
  // simultaneously reading via the SDDP state-variable propagation
  // (`get_col_sol` -> `get_numcols()`).  `release_backend` resets the
  // unique_ptr AFTER
  // populating the cache (line ~214 sets `m_cache_.numcols`), so a
  // reader that observes a null `m_backend_` is guaranteed the cache
  // already carries the right value.  Without the null check this
  // function null-derefed at `m_backend_->get_num_cols()` and crashed
  // the run on juan/iplp at iter 4 with default `max_async_spread=2`
  // (full stack trace + bisect: gdb session 2026-05-05).
  if (m_backend_released_ || !m_backend_) {
    return m_cache_.numcols();
  }
  return m_backend_->get_num_cols();
}

void LinearInterface::set_continuous(const ColIndex index)
{
  ensure_backend();
  m_backend_->set_continuous(index);
}

void LinearInterface::set_integer(const ColIndex index)
{
  ensure_backend();
  m_backend_->set_integer(index);
}

void LinearInterface::set_binary(const ColIndex index)
{
  set_integer(index);
  set_col_low_raw(index, 0);
  set_col_upp_raw(index, 1);
}

bool LinearInterface::is_continuous(const ColIndex index) const
{
  return m_backend_->is_continuous(index);
}

bool LinearInterface::is_integer(const ColIndex index) const
{
  return m_backend_->is_integer(index);
}

Index LinearInterface::relax_integers()
{
  // Ensure a live backend before querying / mutating column types.
  // Under compress / rebuild this rehydrates the snapshot so that the
  // post-snapshot integer flags installed by `load_flat` (see
  // `for (auto i : flat_lp.colint)` above) are visible to the
  // backend's `relax_all_integers` override.
  //
  // Routes through the backend's bulk relaxation virtual: each plugin
  // implements `relax_all_integers()` with its native single-call API
  // (CPLEX `CPXchgprobtype`, HiGHS `clearIntegrality`, Gurobi
  // `GRBsetcharattrarray`, OSI `setContinuous(const int*, int)`,
  // MindOpt default loop), so this is one backend call instead of
  // a per-column dispatch loop on the hot SDDP aperture path.
  ensure_backend();
  return Index {m_backend_->relax_all_integers()};
}

Index LinearInterface::restore_integers(const std::span<const int> integer_cols)
{
  ensure_backend();
  return Index {m_backend_->restore_integers(integer_cols)};
}

bool LinearInterface::has_integer_cols() const
{
  // `is_integer` reads the live backend flags; trigger a reconstruct
  // under compress / rebuild so post-snapshot integrality (installed by
  // `load_flat` from `flat_lp.colint`) is visible.  Const method, but
  // `ensure_backend()` is non-const — route through the mutable
  // `backend()` accessor which performs the reconstruct internally.
  const auto n = backend().get_num_cols();
  for (int i = 0; i < n; ++i) {
    if (m_backend_->is_integer(i)) {
      return true;
    }
  }
  return false;
}

// ── fix_integers_and_resolve ─ moved to linear_interface_solve.cpp
//   (delegates to the backend's `fix_mip_and_resolve_duals` virtual; lives
//    next to `resolve` so it shares the effective-options helper).

// ── Names & LP file output ─ moved to linear_interface_labels.cpp
// ── Solve ─ moved to linear_interface_solve.cpp
//
// `generate_labels_from_maps`, `materialize_labels`,
// `compress_labels_meta_if_needed`, `ensure_labels_meta_decompressed`,
// `push_names_to_solver`, `write_lp` live in
// `source/linear_interface_labels.cpp`.
//
// `initial_solve`, `resolve`, and the algorithm-fallback helper
// `next_fallback_algo` live in `source/linear_interface_solve.cpp`.

// ── Lazy crossover ──

void LinearInterface::ensure_duals()
{
  // Some LP states have no duals to ensure — this is normal, not an
  // error.  Callers must handle the "no duals available" outcome by
  // observing an empty cache span on the next read.
  //
  //   (a) Backend released under `LowMemoryMode::compress` and the
  //       cache was wiped (`invalidate_cached_optimal_on_mutation`)
  //       between the last solve and the release — typical for cells
  //       whose last op was a Benders `add_row` with no subsequent
  //       resolve.  Reconstructing the backend just to re-solve for
  //       duals would force a full re-solve per call, which is
  //       prohibitively expensive at cascade-transition
  //       `update_stored_cut_duals` time (every L0 cell would
  //       otherwise re-solve here).  Accept the missing duals — the
  //       caller's prior `cut.dual` (captured at cut creation) is
  //       the right fallback.
  //
  //   (b) Backend null pre-`load_flat` / mid-shutdown.
  //
  if (m_backend_ == nullptr || m_backend_released_) {
    return;
  }
  // The solve already produced usable duals; `crossover` is the single knob:
  //
  //   * simplex, or barrier WITH crossover (the default) — vertex (basic)
  //     reduced costs / row prices are available by construction.
  //   * barrier WITHOUT crossover (`crossover == false`) — the solve stops at
  //     the interior point, but CPLEX/HiGHS still expose the **interior**
  //     reduced costs (`CplexSolverBackend::reduced_cost()` → `CPXgetdj`).
  //     Those are a valid optimal dual (the unique analytic-center dual ⇒ a
  //     valid value-function subgradient), so they are used DIRECTLY.
  //
  // `ensure_duals()` therefore performs no lazy crossover re-solve: a caller
  // that sets `crossover == false` is taken at its word ("the interior duals
  // are fine").  This is what makes barrier/no-crossover SDDP reproducible
  // across `low_memory` modes — a crossover re-solve here would recover
  // BASIS-DEPENDENT vertex duals, non-unique under degeneracy, reintroducing
  // the off↔compress cut divergence.  Callers needing basic duals leave
  // `crossover` at its `true` default.
}

// ── Status ──

int LinearInterface::get_status() const
{
  if (m_backend_released_) {
    return m_cache_.is_optimal() ? 0 : -1;
  }
  try {
    if (m_backend_->is_proven_optimal()) {
      return 0;
    }
    if (m_backend_->is_abandoned()) {
      return 1;
    }
    if (m_backend_->is_proven_dual_infeasible()
        || m_backend_->is_proven_primal_infeasible())
    {
      return 2;
    }
    return 3;
  } catch (...) {
    return 1;
  }
}

std::optional<double> LinearInterface::get_kappa() const
{
  // Prefer the solve-time cache when available so a later backend
  // re-query (which some backends answer with a recomputed / stale
  // value) cannot perturb downstream readers.  Falls through to the
  // live backend only when the cache hasn't been populated yet
  // (pre-solve reads, LP under construction).
  if (const auto& cached = m_cache_.kappa(); cached.has_value()) {
    return cached;
  }
  if (m_backend_released_) {
    return std::nullopt;
  }
  return m_backend_->get_kappa();
}

RowDiagnostics LinearInterface::diagnose_row(const RowIndex row) const
{
  const auto ncols = get_numcols();

  RowDiagnostics diag {
      .row = row,
  };

  // Row name (if available)
  const auto& rin = *m_row_index_to_name_;
  if (static_cast<size_t>(row) < rin.size()) {
    diag.name = rin[row];
  }

  // Row bounds (raw LP units)
  const auto row_lb = std::span(m_backend_->row_lower(), get_numrows());
  const auto row_ub = std::span(m_backend_->row_upper(), get_numrows());
  diag.rhs_lb = row_lb[row];
  diag.rhs_ub = row_ub[row];

  // Scan all columns for non-zero coefficients in this row
  const auto& cin = *m_col_index_to_name_;
  for (const auto col : iota_range<ColIndex>(0, ncols)) {
    const double v = get_coeff_raw(row, col);
    if (v == 0.0) {
      continue;
    }
    const double abs_v = std::abs(v);
    ++diag.num_nonzeros;

    const auto& col_name =
        (static_cast<size_t>(col) < cin.size()) ? cin[col] : "";

    if (abs_v < diag.min_abs_coeff) {
      diag.min_abs_coeff = abs_v;
      diag.min_col_name = col_name;
    }
    if (abs_v > diag.max_abs_coeff) {
      diag.max_abs_coeff = abs_v;
      diag.max_col_name = col_name;
    }
  }

  if (diag.num_nonzeros > 0 && diag.min_abs_coeff > 0.0) {
    diag.coeff_ratio = diag.max_abs_coeff / diag.min_abs_coeff;
  }

  return diag;
}

bool LinearInterface::is_optimal() const
{
  // **Off-mode invariant (I6):** off owns no LI-side cache — query
  // the live backend directly.  Under off the backend is always
  // alive (release_backend is a no-op), so this is a single in-line
  // CPLEX call (effectively free) and avoids carrying a parallel
  // cached flag whose only purpose would be to mirror the backend.
  if (m_low_memory_mode_ == LowMemoryMode::off) {
    return m_backend_ != nullptr && m_backend_->is_proven_optimal();
  }
  // Compress/snapshot: the LI-side cached flag is the source of truth.
  // Set inside `populate_solution_cache_post_solve` (eagerly post-solve)
  // and cleared inside `invalidate_cached_optimal_on_mutation` on every
  // structural mutation.  This survives `release_backend()` —
  // critical for cross-phase reads in SDDP backward.
  return m_cache_.is_optimal();
}

bool LinearInterface::is_dual_infeasible() const
{
  if (m_backend_released_) {
    return false;
  }
  return m_backend_->is_proven_dual_infeasible();
}

bool LinearInterface::is_prim_infeasible() const
{
  if (m_backend_released_) {
    return false;
  }
  return m_backend_->is_proven_primal_infeasible();
}

double LinearInterface::get_obj_value_raw() const
{
  // Composed raw-scale obj: solver's value plus the LP-external
  // `obj_constant_raw` (algebraic constants from variable
  // substitutions, e.g. the P0 demand-failure `fail = lmax − load`
  // rewrite).  Adding here — rather than only in `get_obj_value()`
  // — keeps pre-substitution test assertions on `get_obj_value_raw()`
  // bit-stable across formulations: the raw view reflects the
  // algebraically-equivalent objective the pre-rewrite LP would
  // have reported.
  const double solver_raw =
      m_backend_released_ ? m_cache_.obj_value() : m_backend_->obj_value();
  return solver_raw + m_obj_constant_raw_;
}

double LinearInterface::get_obj_value() const
{
  // Physical (post-`scale_objective`) view: simply the raw value
  // multiplied back up by `scale_objective`.  `obj_constant_raw`
  // is already folded into `get_obj_value_raw()`, so multiplying
  // gives `(solver_raw + obj_constant_raw) × scale_objective`
  // = `solver_raw × scale_objective + obj_constant_phys`, matching
  // the algebraic objective of the pre-substitution formulation.
  return get_obj_value_raw() * m_scale_objective_;
}

void LinearInterface::add_obj_constant(double c) noexcept
{
  // Convert physical (caller-facing) → raw (storage) scale.  Mirrors
  // the `m_obj_constant_ / scale_obj` conversion done at `flatten()`
  // time so that the running `m_obj_constant_raw_` always lives on
  // the same basis as the solver's value, letting
  // `get_obj_value_raw()` compose them with a plain add.
  const double c_raw = c / m_scale_objective_;
  m_obj_constant_raw_ += c_raw;

  // Mirror the addition into the held snapshot's flat LP.  Scalar
  // fields on `FlatLinearProblem` (incl. `obj_constant_raw`,
  // `scale_objective`, `nrows`, `ncols`) survive compression /
  // decompression — only the bulk vectors are encoded into the
  // compressed buffer (see `compress_flat_lp` /
  // `decompress_flat_lp` in `memory_compress.cpp`).  This single
  // line therefore covers both snapshot states, ensuring a later
  // `load_flat` reconstruct re-establishes the same raw constant.
  if (m_snapshot_holder_.has_data()) {
    m_snapshot_holder_.snapshot_mut().flat_lp.obj_constant_raw += c_raw;
  }
}

void LinearInterface::set_col_sol(const std::span<const double> sol)
{
  if (sol.data() != nullptr) {
    m_backend_->set_col_solution(sol.data());
  }
}

void LinearInterface::set_row_dual(const std::span<const double> dual)
{
  if (dual.data() != nullptr) {
    m_backend_->set_row_price(dual.data());
  }
}

bool LinearInterface::set_mip_start(const std::span<const double> col_values,
                                    const MipStartEffort effort)
{
  ensure_backend();
  return m_backend_->set_mip_start(col_values, effort);
}

std::optional<std::vector<std::string>> LinearInterface::diagnose_infeasibility(
    const int max_items)
{
  ensure_backend();
  auto raw = m_backend_->diagnose_infeasibility(max_items);
  if (!raw) {
    return raw;
  }
  // The backend reports a generic "row_<N>" when the solver holds no row
  // names; enrich each with this interface's own row label (class /
  // constraint name) so the conflict is human-readable (e.g. a Commitment
  // min-down-time row).
  constexpr std::string_view prefix {"row_"};
  for (auto& entry : *raw) {
    if (!entry.starts_with(prefix)) {
      continue;
    }
    // Parse the "row_<N>" numeric suffix; skip anything that isn't a
    // clean all-digits index (std::from_chars rejects junk + overflow).
    const auto digits = std::string_view {entry}.substr(prefix.size());
    int idx = 0;
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), idx);
    if (ec != std::errc {} || ptr != digits.data() + digits.size()) {
      continue;
    }
    if (const auto* lbl = row_label_at(RowIndex {static_cast<Index>(idx)});
        lbl != nullptr && !lbl->constraint_name.empty())
    {
      entry += std::format(" [{}:{}]", lbl->class_name, lbl->constraint_name);
    }
  }
  return raw;
}

}  // namespace gtopt
