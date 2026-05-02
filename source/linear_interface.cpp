/**
 * @file      linear_interface.cpp
 * @brief     LinearInterface implementation — solver-agnostic via SolverBackend
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_interface_labels_codec.hpp>
#include <gtopt/lp_equilibration.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>  // complete type for m_rebuild_owner_->rebuild_in_place()
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

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::invoke_rebuild_owner()
{
  m_rebuild_owner_->rebuild_in_place();
}

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
  m_cached_obj_value_ = m_backend_->obj_value();
  m_cached_kappa_ = m_backend_->get_kappa();
  m_cached_numrows_ = get_numrows();
  m_cached_numcols_ = get_numcols();
  m_cached_is_optimal_ = true;
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::ensure_backend()
{
  if (!m_backend_released_) {
    return;
  }
  // Rebuild mode: the owning SystemLP regenerates the flat LP from
  // source collections, loads it onto this interface, and replays
  // persistent state (dynamic cols + active cuts).  Mirrors compress
  // mode's transparent reconstruct, but sources the flat LP from the
  // element collections rather than a snapshot.
  //
  // Invariant: if no owner is installed, this is a bare
  // `LinearInterface` without a parent SystemLP (unit tests).  Silent
  // short-circuit keeps those tests working.
  if (m_low_memory_mode_ == LowMemoryMode::rebuild) {
    if (m_rebuilding_ || m_rebuild_owner_ == nullptr) {
      return;
    }
    m_rebuilding_ = true;
    try {
      invoke_rebuild_owner();  // flatten → load_flat → apply_post_load_replay
    } catch (...) {
      m_rebuilding_ = false;
      throw;
    }
    m_rebuilding_ = false;
    return;
  }
  // Snapshot/compress: reconstruct from the saved flat LP snapshot.
  // No warm-start — callers that want primal/dual continuity must
  // pass their own vectors via `reconstruct_backend(col_sol, row_dual)`.
  reconstruct_backend();
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

  // Cache post-solve scalars for transparent read access.  Full
  // primal/dual vectors are not cached; callers that need them must
  // read before release or trigger ensure_backend() to reload.
  try {
    if (m_backend_ && is_optimal()) {
      m_cached_obj_value_ = m_backend_->obj_value();
      m_cached_kappa_ = m_backend_->get_kappa();
      m_cached_numrows_ = get_numrows();
      m_cached_numcols_ = get_numcols();
      m_cached_is_optimal_ = true;

      // Snapshot primal + dual + reduced-cost vectors so that read-only
      // consumers (OutputContext, Benders cut assembly, SDDP state
      // propagation) can access the solution without forcing a backend
      // reconstruct + re-solve.  The uncompressed cost is ~8 bytes per
      // col/row per cell — negligible against the flat-LP snapshot.
      const auto* col_sol_ptr = m_backend_->col_solution();
      const auto* col_cost_ptr = m_backend_->reduced_cost();
      const auto* row_dual_ptr = m_backend_->row_price();
      if (col_sol_ptr != nullptr) {
        const std::span col_sol {col_sol_ptr, m_cached_numcols_};
        m_cached_col_sol_.assign(col_sol.begin(), col_sol.end());
      }
      if (col_cost_ptr != nullptr) {
        const std::span col_cost {col_cost_ptr, m_cached_numcols_};
        m_cached_col_cost_.assign(col_cost.begin(), col_cost.end());
      }
      if (row_dual_ptr != nullptr) {
        const std::span row_dual {row_dual_ptr, m_cached_numrows_};
        m_cached_row_dual_.assign(row_dual.begin(), row_dual.end());
      }
    } else {
      // Non-optimal release: drop any stale primal/dual snapshot so
      // future `get_col_sol()` reads don't return values that belong
      // to a previous LP state (e.g. before `add_row` mutated the
      // system).  Keeps the getter invariant simple: cached vectors
      // are valid iff populated.
      m_cached_col_sol_.clear();
      m_cached_col_sol_.shrink_to_fit();
      m_cached_col_cost_.clear();
      m_cached_col_cost_.shrink_to_fit();
      m_cached_row_dual_.clear();
      m_cached_row_dual_.shrink_to_fit();
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
      m_cached_is_optimal_ = false;
    }
    // Snapshot/compress: first call compresses the flat LP (one-time,
    // creates a persistent buffer); subsequent calls free the decompressed
    // vectors.  Rebuild mode keeps no snapshot — nothing to compress.
    if (m_low_memory_mode_ != LowMemoryMode::rebuild) {
      if (!m_snapshot_.is_compressed()) {
        enable_compression();
      } else {
        clear_flat_lp_vectors(m_snapshot_.flat_lp);
      }
      // Also compress the label-metadata vectors.  `noexcept` contract
      // on `release_backend` means we swallow any compression error
      // here; next `generate_labels_from_maps` would fall back to
      // whatever is still live (nothing, by design) — acceptable
      // worst-case is `write_lp` throwing on missing metadata, which
      // the caller is already defensive against.
      try {
        compress_labels_meta_if_needed();
      } catch (...) {  // NOLINT(bugprone-empty-catch)
        // Best-effort — proceed with release.
      }
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Best-effort: proceed with release even if caching fails. The
    // function is noexcept, so swallowing exceptions here is intentional —
    // a failure to cache the solution must not break shutdown ordering.
  }

  m_backend_.reset();
  m_backend_released_ = true;
  m_phase_ = LiPhase::BackendReleased;
}

// ── Low-memory mode ──

void LinearInterface::set_low_memory(LowMemoryMode mode,
                                     CompressionCodec codec) noexcept
{
  m_low_memory_mode_ = mode;
  m_memory_codec_ = codec;

  if (mode == LowMemoryMode::off) {
    m_snapshot_ = {};
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
  assert(m_active_cuts_.empty()
         && "freeze_for_cuts: cut additions detected before "
            "structural-build commit");

  set_low_memory(mode, codec);
  // Skip the snapshot for modes that never use it: `off` (no
  // reconstruct path) and `rebuild` (uses the rebuild callback,
  // not the snapshot — see `reconstruct_backend` line ~315 which
  // explicitly asserts `m_low_memory_mode_ != rebuild`).  Other
  // modes (`compress`, `snapshot`) genuinely depend on the
  // snapshot to rehydrate the backend.
  if (mode != LowMemoryMode::off && mode != LowMemoryMode::rebuild) {
    save_snapshot(std::move(flat_lp));
  }
  save_base_numrows();
  m_phase_ = LiPhase::Frozen;
}

void LinearInterface::save_snapshot(FlatLinearProblem flat_lp)
{
  m_snapshot_.flat_lp = std::move(flat_lp);
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
  m_cached_numrows_ = static_cast<size_t>(flat_lp.nrows);
  m_cached_numcols_ = static_cast<size_t>(flat_lp.ncols);

  m_snapshot_.flat_lp = std::move(flat_lp);

  if (m_low_memory_mode_ == LowMemoryMode::compress
      && !m_snapshot_.is_compressed())
  {
    enable_compression();
  }

  // No backend was ever created, so there is nothing to free.
  // Just flip the released flag so ensure_backend() reconstructs lazily.
  m_backend_released_ = true;
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::reconstruct_backend(std::span<const double> col_sol,
                                          std::span<const double> row_dual)
{
  // Rebuild mode never installs a snapshot, so reconstructing from one
  // would be a logic error.  Catch it loudly in debug builds; in
  // release the !has_data() guard below still short-circuits cleanly.
  assert(m_low_memory_mode_ != LowMemoryMode::rebuild
         && "rebuild mode uses the rebuild callback, not reconstruct_backend");
  if (!m_backend_released_ || !m_snapshot_.has_data()) {
    return;
  }

  // Level 2: decompress the snapshot
  disable_compression();

  // Mark as not released early to avoid recursion in add_col/add_rows
  m_backend_released_ = false;

  // 1. Reload the base structural LP
  load_flat(m_snapshot_.flat_lp);

  // 2. Replay persistent SDDP state onto the live backend.
  apply_post_load_replay(col_sol, row_dual);

  // 3. Free decompressed flat LP vectors — the data is now in the backend.
  //    The compressed buffer stays valid as persistent cache for next
  //    reconstruction.  No re-compression needed.
  if (m_snapshot_.is_compressed()) {
    clear_flat_lp_vectors(m_snapshot_.flat_lp);
  }
  m_phase_ = LiPhase::Reconstructed;
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::install_flat_as_rebuild(const FlatLinearProblem& flat_lp)
{
  // Clear the released flag BEFORE load_flat so the replay's add_col /
  // add_rows calls bypass the rebuild re-entry in ensure_backend().  The
  // rebuilding guard in ensure_backend also short-circuits, but clearing
  // the flag lets this method work outside the rebuild callback too
  // (e.g. future explicit callers).
  m_backend_released_ = false;
  load_flat(flat_lp);
  apply_post_load_replay();
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::apply_post_load_replay(std::span<const double> col_sol,
                                             std::span<const double> row_dual)
{
  // Pre-condition: caller has already run `load_flat` and cleared
  // `m_backend_released_` so the add_col/add_rows below don't re-enter
  // `ensure_backend`.
  assert(!m_backend_released_
         && "apply_post_load_replay requires a live backend");

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
  //
  // `add_cols_raw` is available as a sibling API for future callers
  // that explicitly want to skip the scale-vector mutation.
  if (!m_dynamic_cols_.empty()) {
    add_cols(m_dynamic_cols_);
  }

  // 2. Replay structural rows that were added after the snapshot was
  //    taken (e.g. cascade elastic-target constraints).  These must
  //    land BEFORE `save_base_numrows()` so that the structural-vs-
  //    cut boundary counts them as structural — otherwise SDDP cut
  //    accounting (m_base_numrows_) is off by `m_dynamic_rows_.size()`
  //    and `record_cut_deletion` indexes the wrong rows.
  if (!m_dynamic_rows_.empty()) {
    add_rows(m_dynamic_rows_);
  }

  // 3. Mark the structural-vs-cuts boundary.
  save_base_numrows();

  // 4. Bulk-add active cuts (single efficient call).
  replay_active_cuts();

  // 4. Warm-start from the caller-supplied primal/dual when available.
  if (!col_sol.empty() || !row_dual.empty()) {
    set_warm_start_solution(col_sol, row_dual);
  }
}

void LinearInterface::record_dynamic_col(SparseCol col)
{
  if (m_low_memory_mode_ != LowMemoryMode::off) {
    m_dynamic_cols_.push_back(std::move(col));
  }
}

bool LinearInterface::update_dynamic_col_lowb(std::string_view class_name,
                                              std::string_view variable_name,
                                              double new_lowb) noexcept
{
  for (auto& col : m_dynamic_cols_) {
    if (col.class_name == class_name && col.variable_name == variable_name) {
      col.lowb = new_lowb;
      return true;
    }
  }
  return false;
}

bool LinearInterface::update_dynamic_col_bounds(std::string_view class_name,
                                                std::string_view variable_name,
                                                double new_lowb,
                                                double new_uppb) noexcept
{
  for (auto& col : m_dynamic_cols_) {
    if (col.class_name == class_name && col.variable_name == variable_name) {
      col.lowb = new_lowb;
      col.uppb = new_uppb;
      return true;
    }
  }
  return false;
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
// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::replay_active_cuts()
{
  if (m_active_cuts_.empty()) {
    return;
  }
  add_rows(m_active_cuts_);
}

void LinearInterface::record_dynamic_row(SparseRow row)
{
  if (m_low_memory_mode_ != LowMemoryMode::off) {
    m_dynamic_rows_.push_back(std::move(row));
  }
}

void LinearInterface::record_cut_row(SparseRow row)
{
  if (m_low_memory_mode_ != LowMemoryMode::off) {
    m_active_cuts_.push_back(std::move(row));
  }
}

void LinearInterface::record_cut_deletion(std::span<const int> deleted_indices)
{
  if (m_low_memory_mode_ == LowMemoryMode::off || m_active_cuts_.empty()) {
    return;
  }

  const auto base = static_cast<int>(m_base_numrows_);

  std::vector<size_t> offsets;
  offsets.reserve(deleted_indices.size());
  for (const auto idx : deleted_indices) {
    const auto off = static_cast<size_t>(idx - base);
    if (off < m_active_cuts_.size()) {
      offsets.push_back(off);
    }
  }
  std::ranges::sort(offsets, std::greater {});

  for (const auto off : offsets) {
    m_active_cuts_.erase(m_active_cuts_.begin() + static_cast<ptrdiff_t>(off));
  }
}

// ── Compression control ──

void LinearInterface::disable_compression()
{
  if (m_low_memory_mode_ != LowMemoryMode::compress) {
    return;
  }
  m_snapshot_.decompress();
}

void LinearInterface::enable_compression()
{
  if (m_low_memory_mode_ != LowMemoryMode::compress) {
    return;
  }
  const bool was_first_compress = !m_snapshot_.is_compressed();
  m_snapshot_.compress(m_memory_codec_);

  if (was_first_compress && m_snapshot_.is_compressed()) {
    const auto orig = m_snapshot_.compressed_lp.original_size;
    const auto comp = m_snapshot_.compressed_lp.data.size();
    const auto ratio =
        comp > 0 ? static_cast<double>(orig) / static_cast<double>(comp) : 0.0;
    SPDLOG_DEBUG(
        "  snapshot compressed: {} -> {} bytes (ratio {:.2f}x, codec {})",
        orig,
        comp,
        ratio,
        codec_name(m_snapshot_.compressed_lp.codec));
    static std::once_flag first_log_flag;
    std::call_once(first_log_flag,
                   [&]
                   {
                     SPDLOG_INFO(
                         "  first snapshot compressed: {} -> {} bytes "
                         "(ratio {:.2f}x, codec {})",
                         orig,
                         comp,
                         ratio,
                         codec_name(m_snapshot_.compressed_lp.codec));
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
  cloned.m_equilibration_method_ = m_equilibration_method_;
  cloned.m_base_numrows_ = m_base_numrows_;
  cloned.m_base_numrows_set_ = m_base_numrows_set_;
  cloned.m_log_tag_ = m_log_tag_;
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
    cloned.m_col_labels_meta_ = deep_copy_ptr(m_col_labels_meta_);
    cloned.m_row_labels_meta_ = deep_copy_ptr(m_row_labels_meta_);
    cloned.m_col_meta_index_ = deep_copy_ptr(m_col_meta_index_);
    cloned.m_row_meta_index_ = deep_copy_ptr(m_row_meta_index_);
    cloned.m_col_names_ = deep_copy_ptr(m_col_names_);
    cloned.m_row_names_ = deep_copy_ptr(m_row_names_);
    cloned.m_col_index_to_name_ = deep_copy_ptr(m_col_index_to_name_);
    cloned.m_row_index_to_name_ = deep_copy_ptr(m_row_index_to_name_);
  } else {  // shallow — atomic incref, source and clone share state
    cloned.m_variable_scale_map_ = m_variable_scale_map_;
    cloned.m_col_scales_ = m_col_scales_;
    cloned.m_row_scales_ = m_row_scales_;
    cloned.m_col_labels_meta_ = m_col_labels_meta_;
    cloned.m_row_labels_meta_ = m_row_labels_meta_;
    cloned.m_col_meta_index_ = m_col_meta_index_;
    cloned.m_row_meta_index_ = m_row_meta_index_;
    cloned.m_col_names_ = m_col_names_;
    cloned.m_row_names_ = m_row_names_;
    cloned.m_col_index_to_name_ = m_col_index_to_name_;
    cloned.m_row_index_to_name_ = m_row_index_to_name_;
  }
  return cloned;
}

LinearInterface LinearInterface::clone_from_flat(CloneKind kind) const
{
  // Pre-conditions: the manual route reads from `m_snapshot_.flat_lp`,
  // which is only populated when `freeze_for_cuts` ran with
  // `LowMemoryMode::compress` (or `snapshot`).  Compressed snapshots
  // are not directly readable here — the SDDP aperture path
  // decompresses around the backward pass via `DecompressionGuard`
  // (sddp_aperture_pass.cpp:390, 579), so callers reaching this
  // method during the aperture window are guaranteed an
  // uncompressed snapshot.
  if (!m_snapshot_.has_data()) {
    throw std::runtime_error(
        "LinearInterface::clone_from_flat: source has no snapshot — call "
        "freeze_for_cuts(LowMemoryMode::compress) on the source first, or "
        "fall back to clone(kind) which uses the backend's native clone.");
  }
  if (m_snapshot_.is_compressed()) {
    throw std::runtime_error(
        "LinearInterface::clone_from_flat: source snapshot is compressed; "
        "decompress first (DecompressionGuard) or fall back to "
        "clone(kind).");
  }

  // Build the clone via load_flat — no backend.clone() call, no
  // process-global solver side effects, no mutex needed.
  LinearInterface cloned {m_solver_name_, m_log_file_};
  cloned.load_flat(m_snapshot_.flat_lp);

  // Copy the LI-side runtime fields that load_flat doesn't restore.
  // These are normally inherited by the native clone() path on the
  // line that does `cloned.m_scale_objective_ = m_scale_objective_`
  // etc.  Mirror that block here so the two clone routes produce
  // equivalent state for downstream consumers.
  cloned.m_base_numrows_ = m_base_numrows_;
  cloned.m_base_numrows_set_ = m_base_numrows_set_;
  cloned.m_log_tag_ = m_log_tag_;
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
    cloned.m_col_labels_meta_ = m_col_labels_meta_;
    cloned.m_row_labels_meta_ = m_row_labels_meta_;
    cloned.m_col_meta_index_ = m_col_meta_index_;
    cloned.m_row_meta_index_ = m_row_meta_index_;
    cloned.m_col_names_ = m_col_names_;
    cloned.m_row_names_ = m_row_names_;
    cloned.m_col_index_to_name_ = m_col_index_to_name_;
    cloned.m_row_index_to_name_ = m_row_index_to_name_;
  }
  return cloned;
}

void LinearInterface::set_warm_start_solution(
    const std::span<const double> col_sol,
    const std::span<const double> row_dual)
{
  if (!col_sol.empty()) {
    const auto ncols = get_numcols();
    if (col_sol.size() >= ncols) {
      set_col_sol(col_sol.first(ncols));
    } else {
      std::vector<double> padded(ncols, 0.0);
      std::ranges::copy(col_sol, padded.begin());
      set_col_sol(padded);
    }
  }
  if (!row_dual.empty()) {
    const auto nrows = get_numrows();
    if (row_dual.size() >= nrows) {
      set_row_dual(row_dual.first(nrows));
    } else {
      std::vector<double> padded(nrows, 0.0);
      std::ranges::copy(row_dual, padded.begin());
      set_row_dual(padded);
    }
  }
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

  // Preserve per-column scale factors from LinearProblem.
  detach_for_write(m_col_scales_)
      .assign(flat_lp.col_scales.begin(), flat_lp.col_scales.end());

  // Preserve per-row equilibration scale factors (empty when disabled).
  detach_for_write(m_row_scales_)
      .assign(flat_lp.row_scales.begin(), flat_lp.row_scales.end());

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
  detach_for_write(m_col_labels_meta_) = flat_lp.col_labels_meta;
  detach_for_write(m_row_labels_meta_) = flat_lp.row_labels_meta;

  // The flat LP carries the authoritative structural labels.  Drop any
  // stale compressed buffer left over from a previous release_backend()
  // cycle so `ensure_labels_meta_decompressed()` doesn't later try to
  // overlay outdated metadata on top of the freshly-loaded vectors.
  m_col_labels_meta_compressed_ = {};
  m_row_labels_meta_compressed_ = {};
  m_col_labels_meta_count_ = 0;
  m_row_labels_meta_count_ = 0;

  // Hand off the eager dedup maps directly from the FlatLinearProblem
  // (LinearProblem already built them incrementally during add_col /
  // add_row).  Skips the full rehash that `rebuild_meta_indexes()`
  // would otherwise do — saves ~50 ms on 500K-col / 300K-row LPs.
  // `detach_for_write` is a no-op here in practice (load_flat runs
  // on the source LP before any clones exist), but the helper is
  // correct under future lifecycle reorderings.
  detach_for_write(m_col_meta_index_) = flat_lp.col_meta_index;
  detach_for_write(m_row_meta_index_) = flat_lp.row_meta_index;

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
  // `LowMemoryMode::compress` / `rebuild`, `m_backend_` may have been
  // released; `ensure_backend()` lazily reconstructs it.  The assert
  // doubles as a hint to clang-static-analyzer, which otherwise can't
  // see through the rebuild callback and flags every subsequent
  // `m_backend_->…` as a potential null dereference.
  ensure_backend();
  assert(m_backend_ != nullptr);
  const auto index = m_backend_->get_num_cols();
  const auto col = ColIndex {index};

  // Validation hooks (Phase 2): note each finite bound; sentinels
  // (±solver_infinity) are skipped by `note_bound` itself via its
  // `std::isfinite` guard, but the explicit `solver_infinity` cap
  // also catches DblMax-style sentinels passed from CLI tests.
  if (m_validation_options_.effective_enable()) {
    const double infy = m_backend_->infinity();
    const double infy_guard = infy * 0.999;
    const auto loc =
        !name.empty() ? name : std::format("col {}", static_cast<int>(col));
    if (std::abs(collb) < infy_guard) {
      m_validation_stats_.note_bound(collb, loc, m_validation_options_);
    }
    if (std::abs(colub) < infy_guard) {
      m_validation_stats_.note_bound(colub, loc, m_validation_options_);
    }
  }

  m_backend_->add_col(normalize_bound(collb), normalize_bound(colub), 0.0);

  // Uniqueness is enforced on metadata (via m_col_meta_index_) in
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

// NOLINTNEXTLINE(misc-no-recursion)
ColIndex LinearInterface::emit_col_to_backend(const SparseCol& col)
{
  ensure_backend();

  const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);
  const auto index = m_backend_->get_num_cols();

  // Validation hooks (Phase 2): note bounds + objective coefficient.
  // Use the column's variable_name when set, else "col <idx>".
  if (m_validation_options_.effective_enable()) {
    const double infy = m_backend_->infinity();
    const double infy_guard = infy * 0.999;
    const auto loc = !col.variable_name.empty()
        ? std::string {col.variable_name}
        : std::format("col {}", index);
    if (std::abs(col.lowb) < infy_guard) {
      m_validation_stats_.note_bound(col.lowb, loc, m_validation_options_);
    }
    if (std::abs(col.uppb) < infy_guard) {
      m_validation_stats_.note_bound(col.uppb, loc, m_validation_options_);
    }
    m_validation_stats_.note_obj(col.cost, loc, m_validation_options_);
  }

  // Apply the global objective scaling to the obj coefficient,
  // matching the per-column / scale_objective divide that
  // `LinearProblem::flatten()` performs at structural-build time
  // (linear_problem.cpp:725-729).  Without this, columns added
  // post-flatten land in the LP with cost `scale_objective` times
  // larger than structural columns' costs — breaking the SDDP
  // backward-pass cut dynamics on `add_alpha_state_var` (juan/
  // gtopt_iplp at scale_obj=1000: LB inflated to 1.5e+19 at iter 1).
  // `col.scale` is left unchanged here because callers pre-apply it
  // to `col.cost` themselves; treating col.scale as a physical
  // multiplier would double-apply.
  m_backend_->add_col(lowb, uppb, col.cost / m_scale_objective_);

  return ColIndex {index};
}

void LinearInterface::track_col_label_meta(ColIndex col_idx,
                                           const SparseCol& col)
{
  // Rehydrate compressed metadata before mutating (compress mode).
  ensure_labels_meta_decompressed();

  // Track label-only metadata so `generate_labels_from_maps` can
  // synthesise the label on demand at `write_lp` time.  No eager
  // `LabelMaker::make_col_label` call — the formatted string is
  // produced exclusively in `generate_labels_from_maps`, cached
  // there, and reused on subsequent `write_lp` invocations
  // (Option B: always lazy + cache on first compute).
  const auto i = static_cast<size_t>(col_idx);
  auto& cm = detach_for_write(m_col_labels_meta_);
  if (cm.size() <= i) {
    cm.resize(i + 1);
  }
  cm[i] = SparseColLabel {
      .class_name = col.class_name,
      .variable_name = col.variable_name,
      .variable_uid = col.variable_uid,
      .context = col.context,
  };

  // Eager metadata-based duplicate detection.  Key is the metadata
  // slot just written into `m_col_labels_meta_`; no separate label
  // is constructed.  Unlabelled cols (no class_name / no
  // variable_name / unknown uid / monostate context) are skipped so
  // structural tests that build unnamed cells don't collide.
  const auto& meta = cm[i];
  if (!is_empty_col_label(meta)) {
    auto& cmi = detach_for_write(m_col_meta_index_);
    auto [it, inserted] = cmi.try_emplace(meta, col_idx);
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

// NOLINTNEXTLINE(misc-no-recursion)
ColIndex LinearInterface::add_col(const SparseCol& col)
{
  const auto col_idx = emit_col_to_backend(col);

  // Register scale if non-unity.
  if (col.scale != 1.0) {
    set_col_scale(col_idx, col.scale);
  }

  track_col_label_meta(col_idx, col);

  return col_idx;
}

// NOLINTNEXTLINE(misc-no-recursion)
ColIndex LinearInterface::add_col_raw(const SparseCol& col)
{
  // Strict invariant: callers must pre-apply any column scaling to
  // col.cost themselves; m_col_scales_ stays frozen post-load_flat
  // so it can be shared via shared_ptr across aperture clones.
  assert(col.scale == 1.0
         && "add_col_raw: non-unit col.scale forbidden — use add_col() "
            "if you need m_col_scales_ to grow");
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
  const auto col_idx = ColIndex {index};

  // Capture the label-meta fields — mirrors `track_col_label_meta`
  // but writes to per-clone-local storage, NOT the shared metadata.
  const SparseColLabel meta {
      .class_name = col.class_name,
      .variable_name = col.variable_name,
      .variable_uid = col.variable_uid,
      .context = col.context,
  };

  // Eager dedup: check both shared (read-only) and per-clone (local)
  // populations.  Mirrors the production path's
  // `m_col_meta_index_.try_emplace` check, scoped to disposable
  // adds.  Unlabelled cols (empty class/variable + unknown_uid +
  // monostate context) are skipped, matching the production path.
  if (!is_empty_col_label(meta)) {
    const auto& cmi = *m_col_meta_index_;
    if (auto sit = cmi.find(meta); sit != cmi.end()) {
      throw std::runtime_error(std::format(
          "Duplicate disposable LP column metadata: class='{}' var='{}' "
          "uid={} (already present at shared col {}, attempted disposable at "
          "col {})",
          meta.class_name,
          meta.variable_name,
          meta.variable_uid,
          sit->second,
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
    const auto& rmi = *m_row_meta_index_;
    if (auto sit = rmi.find(meta); sit != rmi.end()) {
      throw std::runtime_error(std::format(
          "Duplicate disposable LP row metadata: class='{}' cons='{}' "
          "uid={} (already present at shared row {}, attempted disposable at "
          "row {})",
          meta.class_name,
          meta.constraint_name,
          meta.variable_uid,
          sit->second,
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

ColIndex LinearInterface::emit_cols_to_backend(std::span<const SparseCol> cols)
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

  const auto num_cols = static_cast<int>(cols.size());
  std::vector<int> colbeg(static_cast<size_t>(num_cols) + 1, 0);
  std::vector<int> colind;
  std::vector<double> colval;
  std::vector<double> collb(static_cast<size_t>(num_cols));
  std::vector<double> colub(static_cast<size_t>(num_cols));
  std::vector<double> colobj(static_cast<size_t>(num_cols));

  const auto inv_so = 1.0 / m_scale_objective_;
  const bool validate = m_validation_options_.effective_enable();
  const double infy = m_backend_->infinity();
  const double infy_guard = infy * 0.999;
  for (size_t c = 0; c < cols.size(); ++c) {
    const auto& col = cols[c];
    const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);
    collb[c] = lowb;
    colub[c] = uppb;
    colobj[c] = col.cost * inv_so;
    colbeg[c + 1] = static_cast<int>(colind.size());

    // Validation hooks (Phase 2): note bounds + obj per column in
    // the bulk path so bulk callers see the same warnings as
    // singular `add_col(SparseCol)`.
    if (validate) {
      const auto col_idx =
          static_cast<int>(first_col_index) + static_cast<int>(c);
      const auto loc = !col.variable_name.empty()
          ? std::string {col.variable_name}
          : std::format("col {}", col_idx);
      if (std::abs(col.lowb) < infy_guard) {
        m_validation_stats_.note_bound(col.lowb, loc, m_validation_options_);
      }
      if (std::abs(col.uppb) < infy_guard) {
        m_validation_stats_.note_bound(col.uppb, loc, m_validation_options_);
      }
      m_validation_stats_.note_obj(col.cost, loc, m_validation_options_);
    }
  }

  m_backend_->add_cols(num_cols,
                       colbeg.data(),
                       colind.data(),
                       colval.data(),
                       collb.data(),
                       colub.data(),
                       colobj.data());

  return first_col_index;
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::add_cols(std::span<const SparseCol> cols)
{
  // Physical bulk path — mirrors `add_col(SparseCol)`'s shape:
  //   1. dispatch the bulk backend insert via `emit_cols_to_backend`
  //   2. register `col.scale` for any non-unity entries
  //   3. capture label-meta for every column
  //
  // Order matches the singular path: scale first, then label meta.
  const auto first_col_index = emit_cols_to_backend(cols);
  for (size_t c = 0; c < cols.size(); ++c) {
    const auto& col = cols[c];
    const auto col_idx =
        ColIndex {static_cast<int>(first_col_index) + static_cast<int>(c)};
    if (col.scale != 1.0) {
      set_col_scale(col_idx, col.scale);
    }
    track_col_label_meta(col_idx, col);
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::add_cols_raw(std::span<const SparseCol> cols)
{
  // Raw bulk path — mirrors `add_col_raw(SparseCol)`'s shape:
  // strict invariant that every entry carries `col.scale == 1.0`
  // (callers pre-apply any scaling to `col.cost`), then dispatch via
  // the shared `emit_cols_to_backend` helper, then capture label-meta.
  // `m_col_scales_` stays frozen so it can be shared across aperture
  // clones via `std::shared_ptr`.
  for (const auto& col : cols) {
    assert(col.scale == 1.0
           && "add_cols_raw: non-unit col.scale forbidden — use add_cols() "
              "if you need m_col_scales_ to grow");
  }
  const auto first_col_index = emit_cols_to_backend(cols);
  for (size_t c = 0; c < cols.size(); ++c) {
    const auto col_idx =
        ColIndex {static_cast<int>(first_col_index) + static_cast<int>(c)};
    track_col_label_meta(col_idx, cols[c]);
  }
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

  // Uniqueness is enforced on metadata (via m_row_meta_index_) in
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
  // Rehydrate compressed metadata before mutating (compress mode).
  ensure_labels_meta_decompressed();
  const auto i = static_cast<size_t>(row_idx);
  auto& rm = detach_for_write(m_row_labels_meta_);
  if (rm.size() <= i) {
    rm.resize(i + 1);
  }
  rm[i] = SparseRowLabel {
      .class_name = row.class_name,
      .constraint_name = row.constraint_name,
      .variable_uid = row.variable_uid,
      .context = row.context,
  };

  // Eager metadata-based duplicate detection — see the `add_col`
  // companion for rationale.
  const auto& meta = rm[i];
  if (!is_empty_row_label(meta)) {
    auto& rmi = detach_for_write(m_row_meta_index_);
    auto [it, inserted] = rmi.try_emplace(meta, row_idx);
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
    const double infy = m_backend_->infinity();
    const double infy_guard = infy * 0.999;
    const auto loc = !row.constraint_name.empty()
        ? std::string {row.constraint_name}
        : std::format("row {}", get_numrows());
    for (const auto& [col, val] : row.cmap) {
      if (eps > 0.0 && std::abs(val) > 0.0 && std::abs(val) < eps
          && std::abs(val) >= 1e-30)
      {
        m_validation_stats_.note_filtered(val, eps, loc, m_validation_options_);
      } else {
        m_validation_stats_.note_coeff(val, loc, m_validation_options_);
      }
    }
    if (std::abs(row.lowb) < infy_guard) {
      m_validation_stats_.note_rhs(row.lowb, loc, m_validation_options_);
    }
    if (std::abs(row.uppb) < infy_guard) {
      m_validation_stats_.note_rhs(row.uppb, loc, m_validation_options_);
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
  const bool compose_physical =
      is_cut_phase && (have_col_scales || have_equilibration);

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

  // 3. Per-row row-max equilibration, only when the LP was built with
  //    equilibration on.  When the build chose `none` but col_scales
  //    are non-trivial (semantic scales set in flatten()), column
  //    scaling still runs at step 1; the row-max pass is skipped to
  //    match the historical invariant for non-equilibrated LPs.
  if (have_equilibration) {
    double max_abs = 0.0;
    for (const auto v : elements) {
      max_abs = std::max(max_abs, std::abs(v));
    }
    if (max_abs > 0.0 && max_abs != 1.0) {
      const double inv = 1.0 / max_abs;
      for (auto& v : elements) {
        v *= inv;
      }
      if (lb > -infy && lb < infy) {
        lb *= inv;
      }
      if (ub > -infy && ub < infy) {
        ub *= inv;
      }
      composite_scale *= max_abs;
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

// NOLINTNEXTLINE(misc-no-recursion)
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
  //
  // Implementation note: when compose_physical is active we fall back
  // to per-row `add_row` rather than duplicate the ~100-line
  // compose_physical body here.  The bulk speedup is preserved for the
  // common "no col_scales / no equilibration" path (the `add_rows_raw`
  // tail call below); the equilibrated path pays a per-row cost that
  // is fine for the only hot caller (`replay_active_cuts` runs once per
  // cell-rebuild under low_memory_mode=compress, ≤ a few hundred cuts).
  const bool is_cut_phase = m_base_numrows_set_;
  const bool have_col_scales = !m_col_scales_->empty();
  const bool have_equilibration =
      m_equilibration_method_ != LpEquilibrationMethod::none;
  const bool compose_physical =
      is_cut_phase && (have_col_scales || have_equilibration);

  if (compose_physical) {
    for (const auto& row : rows) {
      add_row(row, eps);
    }
    return;
  }

  add_rows_raw(rows, eps);
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

  const auto num_rows = static_cast<int>(rows.size());
  const auto first_row_index = m_backend_->get_num_rows();

  // First pass: count total non-zeros to preallocate CSR arrays.
  size_t total_nnz = 0;
  for (const auto& row : rows) {
    for (const auto& [col, val] : row.cmap) {
      if (std::abs(val) > eps) {
        ++total_nnz;
      }
    }
  }

  // Allocate CSR arrays
  std::vector<int> rowbeg(static_cast<size_t>(num_rows) + 1);
  std::vector<int> rowind;
  std::vector<double> rowval;
  std::vector<double> rowlb(static_cast<size_t>(num_rows));
  std::vector<double> rowub(static_cast<size_t>(num_rows));
  rowind.reserve(total_nnz);
  rowval.reserve(total_nnz);

  const auto infy = m_backend_->infinity();
  const double infy_guard = infy * 0.999;
  const bool validate = m_validation_options_.effective_enable();

  // Second pass: fill CSR arrays with scaled coefficients and bounds.
  for (const auto& [r, row] : enumerate(rows)) {
    rowbeg[r] = static_cast<int>(rowind.size());

    const auto rs = row.scale;
    const auto inv_rs = (rs != 1.0) ? 1.0 / rs : 1.0;

    const std::string row_loc = validate
        ? (!row.constraint_name.empty()
               ? std::string {row.constraint_name}
               : std::format("row {}", first_row_index + static_cast<int>(r)))
        : std::string {};

    for (const auto& [col, val] : row.cmap) {
      const auto scaled = val * inv_rs;
      if (std::abs(scaled) > eps) {
        rowind.push_back(static_cast<int>(col));
        rowval.push_back(scaled);
        if (validate) {
          m_validation_stats_.note_coeff(
              scaled, row_loc, m_validation_options_);
        }
      } else if (validate && eps > 0.0 && std::abs(scaled) > 0.0
                 && std::abs(scaled) >= 1e-30)
      {
        m_validation_stats_.note_filtered(
            scaled, eps, row_loc, m_validation_options_);
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
        m_validation_stats_.note_rhs(lb, row_loc, m_validation_options_);
      }
      if (std::abs(ub) < infy_guard) {
        m_validation_stats_.note_rhs(ub, row_loc, m_validation_options_);
      }
    }
  }
  rowbeg[static_cast<size_t>(num_rows)] = static_cast<int>(rowind.size());

  // Dispatch bulk add to solver backend
  m_backend_->add_rows(num_rows,
                       rowbeg.data(),
                       rowind.data(),
                       rowval.data(),
                       rowlb.data(),
                       rowub.data());

  // Update row scales, label metadata, and name maps for all new rows.
  // `track_row_label_meta` is what keeps `m_row_labels_meta_` in sync with
  // the backend row count — without it, a later `generate_labels_from_maps`
  // call (e.g. through `write_lp` / `push_names_to_solver`) throws
  // "row N has no entry in m_row_labels_meta_".  This bulk path is hit
  // from `apply_post_load_replay` when a low-memory cell rebuilds and
  // re-injects `m_active_cuts_`; the single-row `add_row_raw` path
  // already calls `track_row_label_meta`, so mirroring it here keeps the
  // size invariant across compress/replay cycles.
  for (const auto& [r, row] : enumerate(rows)) {
    const auto row_idx = RowIndex {first_row_index + static_cast<int>(r)};

    if (row.scale != 1.0) {
      set_row_scale(row_idx, row.scale);
    }

    track_row_label_meta(row_idx, row);

    if (m_label_maker_.row_names_enabled()) {
      const auto name = m_label_maker_.make_row_label(row);
      if (!name.empty()) {
        const auto i = static_cast<size_t>(row_idx);
        auto& rin = detach_for_write(m_row_index_to_name_);
        if (rin.size() <= i) {
          rin.resize(i + 1);
        }
        rin[row_idx] = name;
      }
    }
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
  // indices stay valid as we shrink each container.  Touching these
  // here (instead of letting `generate_labels_from_maps` rebuild)
  // keeps `row_name_map()` callers in sync even when they skip the
  // full materialise path.
  // Detach both shared_ptr-wrapped containers once so the erase loop
  // runs against clone-local copies if currently shared.
  auto& rm = detach_for_write(m_row_labels_meta_);
  auto& rin = detach_for_write(m_row_index_to_name_);
  for (const auto idx : indices | std::views::reverse) {
    const auto pos = static_cast<ptrdiff_t>(idx);
    if (pos < std::ssize(rin)) {
      rin.erase(rin.begin() + pos);
    }
    if (pos < std::ssize(rm)) {
      rm.erase(rm.begin() + pos);
    }
  }
  rebuild_row_name_maps();

  // Row indices shifted on deletion, so every previously-registered
  // `(metadata → RowIndex)` pair is now potentially stale.  Rebuild
  // the map from the current `m_row_labels_meta_` (column indices
  // are untouched by `delete_rows`, so `m_col_meta_index_` is
  // preserved — we only regenerate the row side).
  auto& rmi = detach_for_write(m_row_meta_index_);
  rmi.clear();
  rmi.reserve(rm.size());
  for (const auto [i, label] : enumerate<RowIndex>(rm)) {
    if (!label.class_name.empty() || !label.constraint_name.empty()
        || label.variable_uid != unknown_uid
        || !std::holds_alternative<std::monostate>(label.context))
    {
      rmi.emplace(label, i);
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
    m_backend_->set_col_lower(static_cast<int>(c), lo);
    m_backend_->set_col_upper(static_cast<int>(c), src_col_hi[c]);
  }

  const auto nrows = m_backend_->get_num_rows();
  const auto src_row_lo = source.get_row_low_raw();
  const auto src_row_hi = source.get_row_upp_raw();
  for (const auto [r, lo] :
       enumerate<RowIndex>(src_row_lo | std::views::take(nrows)))
  {
    m_backend_->set_row_lower(static_cast<int>(r), lo);
    m_backend_->set_row_upper(static_cast<int>(r), src_row_hi[r]);
  }

  if (m_label_maker_.row_names_enabled()) {
    detach_for_write(m_row_index_to_name_).resize(static_cast<size_t>(nrows));
    rebuild_row_name_maps();
  }
}

// ── Coefficients ──

// ── Raw coefficient accessors (LP units) ──

void LinearInterface::set_coeff_raw(const RowIndex row,
                                    const ColIndex column,
                                    const double value)
{
  ensure_backend();
  if (m_validation_options_.effective_enable()) {
    m_validation_stats_.note_coeff(
        value,
        std::format(
            "row {} col {}", static_cast<int>(row), static_cast<int>(column)),
        m_validation_options_);
  }
  m_backend_->set_coeff(static_cast<int>(row), static_cast<int>(column), value);
}

double LinearInterface::get_coeff_raw(const RowIndex row,
                                      const ColIndex column) const
{
  return m_backend_->get_coeff(static_cast<int>(row), static_cast<int>(column));
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
    m_validation_stats_.note_obj(value,
                                 std::format("col {}", static_cast<int>(index)),
                                 m_validation_options_);
  }
  m_backend_->set_obj_coeff(static_cast<int>(index), value);
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

// ── Raw column bound setters (LP/solver units) ──

void LinearInterface::set_col_low_raw(const ColIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(value) < infy_guard) {
      m_validation_stats_.note_bound(
          value,
          std::format("col {}", static_cast<int>(index)),
          m_validation_options_);
    }
  }
  m_backend_->set_col_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col_upp_raw(const ColIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(value) < infy_guard) {
      m_validation_stats_.note_bound(
          value,
          std::format("col {}", static_cast<int>(index)),
          m_validation_options_);
    }
  }
  m_backend_->set_col_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col_raw(const ColIndex index, const double value)
{
  set_col_low_raw(index, value);
  set_col_upp_raw(index, value);
}

// ── Physical column bound setters (physical_value / col_scale → LP) ──

// Physical bound setters: plain descale-to-raw.  No snap-to-existing-
// bound logic here — it was added in e58add8f to catch solver-tolerance
// noise on equality pins, but the aperture-backward pass calls these
// setters tens of thousands of times per iteration, and the extra
// virtual dispatches (`get_col_low()`, `get_col_upp()`, `infinity()`)
// produced a ~20x slowdown on the Juan gtopt_iplp case (iter 0 went
// from ~35s to ~550s).  The propagation-noise problem that motivated
// the snap is already handled on the *read* side by `get_col_sol()`'s
// optimal-only bound clamp; clean values are in every consumer's hand
// without any work by the setters.
void LinearInterface::set_col_low(const ColIndex index,
                                  const double physical_value)
{
  set_col_low_raw(index, physical_value / get_col_scale(index));
}

void LinearInterface::set_col_upp(const ColIndex index,
                                  const double physical_value)
{
  set_col_upp_raw(index, physical_value / get_col_scale(index));
}

void LinearInterface::set_col(const ColIndex index, const double physical_value)
{
  set_col_low(index, physical_value);
  set_col_upp(index, physical_value);
}

// ── Raw row bound setters (LP/solver units) ──

void LinearInterface::set_row_low_raw(const RowIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(value) < infy_guard) {
      m_validation_stats_.note_rhs(
          value,
          std::format("row {}", static_cast<int>(index)),
          m_validation_options_);
    }
  }
  m_backend_->set_row_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_row_upp_raw(const RowIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(value) < infy_guard) {
      m_validation_stats_.note_rhs(
          value,
          std::format("row {}", static_cast<int>(index)),
          m_validation_options_);
    }
  }
  m_backend_->set_row_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_rhs_raw(const RowIndex row, const double rhs)
{
  // Match the rest of the raw-mutation setters above: ensure the
  // backend is live before delegating.  Without this, a mutation
  // issued after `release_backend()` would null-deref `m_backend_`
  // (flagged by clang-analyzer-core.CallAndMessage).
  ensure_backend();
  if (m_validation_options_.effective_enable()) {
    const double infy_guard = m_backend_->infinity() * 0.999;
    if (std::abs(rhs) < infy_guard) {
      m_validation_stats_.note_rhs(rhs,
                                   std::format("row {}", static_cast<int>(row)),
                                   m_validation_options_);
    }
  }
  m_backend_->set_row_bounds(static_cast<int>(row), rhs, rhs);
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

void LinearInterface::set_rhs(const RowIndex row, const double physical_rhs)
{
  const double scale = get_row_scale(row);
  set_rhs_raw(row, physical_rhs / scale);
}

size_t LinearInterface::get_numrows() const
{
  if (m_backend_released_) {
    return m_cached_numrows_;
  }
  return static_cast<size_t>(m_backend_->get_num_rows());
}

size_t LinearInterface::get_numcols() const
{
  if (m_backend_released_) {
    return m_cached_numcols_;
  }
  return static_cast<size_t>(m_backend_->get_num_cols());
}

void LinearInterface::set_continuous(const ColIndex index)
{
  ensure_backend();
  m_backend_->set_continuous(static_cast<int>(index));
}

void LinearInterface::set_integer(const ColIndex index)
{
  ensure_backend();
  m_backend_->set_integer(static_cast<int>(index));
}

void LinearInterface::set_binary(const ColIndex index)
{
  set_integer(index);
  set_col_low_raw(index, 0);
  set_col_upp_raw(index, 1);
}

bool LinearInterface::is_continuous(const ColIndex index) const
{
  return m_backend_->is_continuous(static_cast<int>(index));
}

bool LinearInterface::is_integer(const ColIndex index) const
{
  return m_backend_->is_integer(static_cast<int>(index));
}

// ── Names & LP file output ─ moved to linear_interface_labels.cpp
// ── Solve ─ moved to linear_interface_solve.cpp
//
// `generate_labels_from_maps`, `materialize_labels`,
// `compress_labels_meta_if_needed`, `ensure_labels_meta_decompressed`,
// `rebuild_meta_indexes`, `push_names_to_solver`, `write_lp` live in
// `source/linear_interface_labels.cpp`.
//
// `initial_solve`, `resolve`, and the algorithm-fallback helper
// `next_fallback_algo` live in `source/linear_interface_solve.cpp`.

// ── Lazy crossover ──

void LinearInterface::ensure_duals()
{
  // Duals are always available unless we solved with barrier w/o crossover.
  // CLP/CBC (simplex-only) always produce vertex duals regardless of options.
  if (m_last_solver_options_.algorithm != LPAlgo::barrier
      || m_last_solver_options_.crossover)
  {
    return;  // solver already has proper vertex duals
  }

  // Re-solve with crossover enabled to obtain vertex duals.
  // The solver warm-starts from the interior-point solution.
  auto opts = m_last_solver_options_;
  opts.crossover = true;
  m_backend_->apply_options(opts);

  ++m_solver_stats_.crossover_solves;
  const auto t0 = std::chrono::steady_clock::now();
  m_backend_->resolve();
  m_solver_stats_.total_solve_time_s +=
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  // Update cached options so subsequent dual accesses don't re-solve.
  m_last_solver_options_.crossover = true;

  SPDLOG_INFO("lazy crossover: computed duals on demand ({})", get_prob_name());
}

// ── Status ──

int LinearInterface::get_status() const
{
  if (m_backend_released_) {
    return m_cached_is_optimal_ ? 0 : -1;
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
  if (m_cached_kappa_.has_value()) {
    return m_cached_kappa_;
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
  diag.rhs_lb = row_lb[static_cast<size_t>(row)];
  diag.rhs_ub = row_ub[static_cast<size_t>(row)];

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
  if (m_backend_released_) {
    return m_cached_is_optimal_;
  }
  return m_backend_->is_proven_optimal();
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
  if (m_backend_released_) {
    return m_cached_obj_value_;
  }
  return m_backend_->obj_value();
}

double LinearInterface::get_obj_value() const
{
  return get_obj_value_raw() * m_scale_objective_;
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

}  // namespace gtopt
