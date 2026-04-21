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
#include <expected>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_equilibration.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>  // complete type for m_rebuild_owner_->rebuild_in_place()
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
/// Check name uniqueness via map insertion (try_emplace pattern).
/// Returns true if the name is a duplicate.  When `errors_on_dup` is true
/// (LabelMaker::duplicates_are_errors() when names are enabled) a
/// std::runtime_error is thrown; otherwise a warning is logged and the
/// function returns true.
///
/// The map is either `col_name_map_t` (keyed to `ColIndex`) or
/// `row_name_map_t` (keyed to `RowIndex`); the index parameter must match.
template<typename Map, typename IndexType>
inline bool check_name_unique(Map& name_map,
                              const std::string& name,
                              IndexType index,
                              std::string_view entity_type,
                              bool errors_on_dup)
{
  if (name.empty()) {
    return false;
  }

  auto [it, inserted] = name_map.try_emplace(name, index);
  if (!inserted) {
    if (errors_on_dup) {
      SPDLOG_ERROR("Duplicate LP {} name: {}", entity_type, name);
      throw std::runtime_error(
          std::format("Duplicate LP {} name: {}", entity_type, name));
    }
    SPDLOG_WARN("Duplicate LP {} name: {}", entity_type, name);
    return true;
  }
  return false;
}

}  // namespace

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
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Best-effort: proceed with release even if caching fails. The
    // function is noexcept, so swallowing exceptions here is intentional —
    // a failure to cache the solution must not break shutdown ordering.
  }

  m_backend_.reset();
  m_backend_released_ = true;
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
  for (const auto& col : m_dynamic_cols_) {
    add_col(col);
  }

  // 2. Mark the structural-vs-cuts boundary.
  save_base_numrows();

  // 3. Bulk-add active cuts (single efficient call).
  if (!m_active_cuts_.empty()) {
    add_rows(m_active_cuts_);
  }

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

LinearInterface LinearInterface::clone() const
{
  // Route through the centralized `backend()` accessor so the source
  // auto-resurrects if released (low_memory_mode != off).  Prior to
  // this, direct `m_backend_->clone()` null-deref segfaulted when the
  // SDDP forward pass had released the phase backend before the
  // backward/aperture pass cloned it.
  auto cloned = LinearInterface {backend().clone(), m_log_file_};
  cloned.m_scale_objective_ = m_scale_objective_;
  cloned.m_col_scales_ = m_col_scales_;
  cloned.m_variable_scale_map_ = m_variable_scale_map_;
  cloned.m_log_tag_ = m_log_tag_;
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
  m_col_scales_.assign(flat_lp.col_scales.begin(), flat_lp.col_scales.end());

  // Preserve per-row equilibration scale factors (empty when disabled).
  m_row_scales_.assign(flat_lp.row_scales.begin(), flat_lp.row_scales.end());

  // Persist the equilibration method so the follow-up PR that migrates
  // Benders cut construction to physical accessors can apply the same
  // per-row scaling to post-build cut rows as the structural build did.
  m_equilibration_method_ = flat_lp.equilibration_method;

  // Preserve VariableScaleMap for dynamic column auto-scaling.
  m_variable_scale_map_ = flat_lp.variable_scale_map;

  // Preserve LabelMaker so dynamically added cols/rows after load_flat()
  // use the same LpNamesLevel as the original flatten() call.
  m_label_maker_ = flat_lp.label_maker;

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
        flat_lp.colnm, m_col_names_, m_col_index_to_name_);
  }

  if (!flat_lp.rownm.empty()) {
    build_name_map.template operator()<RowIndex>(
        flat_lp.rownm, m_row_names_, m_row_index_to_name_);
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
  check_name_unique(m_col_names_,
                    name,
                    col,
                    "column",
                    m_label_maker_.duplicates_are_errors());

  m_backend_->add_col(normalize_bound(collb), normalize_bound(colub), 0.0);

  if (m_label_maker_.col_names_enabled() && !name.empty()) {
    if (m_col_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_col_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_col_index_to_name_[col] = name;
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
ColIndex LinearInterface::add_col(const SparseCol& col)
{
  ensure_backend();
  // Label generation delegated to LabelMaker.  Returns an empty string
  // when LP column names are disabled (LpNamesLevel::none) or when the
  // column lacks the metadata needed to produce a label.
  const auto name = m_label_maker_.make_col_label(col);

  const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);

  const auto index = m_backend_->get_num_cols();
  const auto col_idx = ColIndex {index};
  check_name_unique(m_col_names_,
                    name,
                    col_idx,
                    "column",
                    m_label_maker_.duplicates_are_errors());

  m_backend_->add_col(lowb, uppb, col.cost);

  if (!name.empty()) {
    if (m_col_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_col_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_col_index_to_name_[col_idx] = name;
  }

  // Register scale if non-unity.
  if (col.scale != 1.0) {
    set_col_scale(col_idx, col.scale);
  }

  return col_idx;
}

void LinearInterface::add_cols(std::span<const SparseCol> cols)
{
  for (const auto& col : cols) {
    add_col(col);
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
  check_name_unique(m_row_names_,
                    name,
                    row_idx,
                    "row",
                    m_label_maker_.duplicates_are_errors());

  m_backend_->add_row(static_cast<int>(numberElements),
                      columns.data(),
                      elements.data(),
                      normalize_bound(rowlb),
                      normalize_bound(rowub));

  if (m_label_maker_.row_names_enabled() && !name.empty()) {
    if (m_row_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_row_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_row_index_to_name_[row_idx] = name;
  }

  return row_idx;
}

RowIndex LinearInterface::add_row(const SparseRow& row, const double eps)
{
  ensure_backend();

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
  const bool have_col_scales = !m_col_scales_.empty();
  const bool have_equilibration =
      m_equilibration_method_ != LpEquilibrationMethod::none;
  const bool compose_physical =
      is_cut_phase && (have_col_scales || have_equilibration);

  if (!compose_physical) {
    return add_row_lp_space(row, eps);
  }

  // Physical-space cut insertion — operates directly on the flat
  // (columns, elements) representation to avoid building an
  // intermediate `SparseRow` and to keep the scale composition in one
  // place instead of bouncing through `add_row_lp_space` (which would
  // otherwise re-apply `SparseRow::scale` on top of our already-
  // composed divisor).
  auto [columns, elements] = row.to_flat<int>(eps);
  double lb = row.lowb;
  double ub = row.uppb;
  const auto infy = m_backend_->infinity();

  // 1. Physical → LP column scaling.  Columns beyond the stored
  //    `m_col_scales_` extent default to scale 1.0 (matching
  //    `get_col_scale`).
  const auto n_col_scales = std::ssize(m_col_scales_);
  for (std::size_t k = 0; k < elements.size(); ++k) {
    const auto col = ColIndex {columns[k]};
    if (col < n_col_scales) {
      elements[k] *= m_col_scales_[col];
    }
  }

  // 2. SparseRow::scale composition (divides row contents by row.scale,
  //    same contract as `flatten()` applies to structural rows before
  //    the bulk equilibration pass).
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

  const auto name = m_label_maker_.make_row_label(row);
  const auto row_idx = add_row(name, columns.size(), columns, elements, lb, ub);
  if (composite_scale != 1.0) {
    set_row_scale(row_idx, composite_scale);
  }
  return row_idx;
}

RowIndex LinearInterface::add_row_lp_space(const SparseRow& row,
                                           const double eps)
{
  // Internal raw-insertion path — called by the public `add_row` after
  // it has (optionally) composed col_scales and row-max equilibration
  // for physical-space cuts, or directly when the caller flagged the
  // row as already being in LP space.  No further per-column or per-
  // row equilibration happens here; we only compose `SparseRow::scale`
  // (the caller-specified row scaler, mirroring how `flatten()` handles
  // static rows).
  //
  // Label generation delegated to LabelMaker.  Returns an empty string
  // when LP row names are disabled or the row lacks the metadata needed
  // to produce a label.
  const auto name = m_label_maker_.make_row_label(row);

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

    const auto row_idx =
        add_row(name, columns.size(), columns, elements, lb, ub);
    set_row_scale(row_idx, rs);
    return row_idx;
  }

  const auto [columns, elements] = row.to_flat<int>(eps);
  return add_row(name, columns.size(), columns, elements, row.lowb, row.uppb);
}

// NOLINTNEXTLINE(misc-no-recursion)
void LinearInterface::add_rows(const std::span<const SparseRow> rows,
                               const double eps)
{
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

  // Second pass: fill CSR arrays with scaled coefficients and bounds.
  for (const auto& [r, row] : enumerate(rows)) {
    rowbeg[r] = static_cast<int>(rowind.size());

    const auto rs = row.scale;
    const auto inv_rs = (rs != 1.0) ? 1.0 / rs : 1.0;

    for (const auto& [col, val] : row.cmap) {
      const auto scaled = val * inv_rs;
      if (std::abs(scaled) > eps) {
        rowind.push_back(static_cast<int>(col));
        rowval.push_back(scaled);
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
  }
  rowbeg[static_cast<size_t>(num_rows)] = static_cast<int>(rowind.size());

  // Dispatch bulk add to solver backend
  m_backend_->add_rows(num_rows,
                       rowbeg.data(),
                       rowind.data(),
                       rowval.data(),
                       rowlb.data(),
                       rowub.data());

  // Update row scales and name maps for all new rows
  for (const auto& [r, row] : enumerate(rows)) {
    const auto row_idx = RowIndex {first_row_index + static_cast<int>(r)};

    if (row.scale != 1.0) {
      set_row_scale(row_idx, row.scale);
    }

    if (m_label_maker_.row_names_enabled()) {
      const auto name = m_label_maker_.make_row_label(row);

      check_name_unique(m_row_names_,
                        name,
                        row_idx,
                        "row",
                        m_label_maker_.duplicates_are_errors());

      if (!name.empty()) {
        if (m_row_index_to_name_.size() <= static_cast<size_t>(row_idx)) {
          m_row_index_to_name_.resize(static_cast<size_t>(row_idx) + 1);
        }
        m_row_index_to_name_[row_idx] = name;
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

  if (m_label_maker_.row_names_enabled()) {
    for (const auto idx : indices | std::views::reverse) {
      const auto pos = static_cast<ptrdiff_t>(idx);
      if (pos < std::ssize(m_row_index_to_name_)) {
        m_row_index_to_name_.erase(m_row_index_to_name_.begin() + pos);
      }
    }
    rebuild_row_name_maps();
  }
}

void LinearInterface::rebuild_row_name_maps()
{
  if (m_label_maker_.duplicates_are_errors()) {
    m_row_names_.clear();
    m_row_names_.reserve(m_row_index_to_name_.size());
    for (const auto [i, name] : enumerate<RowIndex>(m_row_index_to_name_)) {
      if (!name.empty()) {
        m_row_names_.emplace(name, i);
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
    m_row_index_to_name_.resize(static_cast<size_t>(nrows));
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
  m_backend_->set_coeff(static_cast<int>(row), static_cast<int>(column), value);
}

double LinearInterface::get_coeff_raw(const RowIndex row,
                                      const ColIndex column) const
{
  return m_backend_->get_coeff(static_cast<int>(row), static_cast<int>(column));
}

// ── Physical coefficient accessors ──
// physical_coeff = raw_coeff × col_scale × row_scale
// raw_coeff = physical_coeff / col_scale / row_scale

void LinearInterface::set_coeff(const RowIndex row,
                                const ColIndex column,
                                const double physical_value)
{
  const double cs = get_col_scale(column);
  const double rs = get_row_scale(row);
  set_coeff_raw(row, column, physical_value / cs / rs);
}

double LinearInterface::get_coeff(const RowIndex row,
                                  const ColIndex column) const
{
  const double raw = get_coeff_raw(row, column);
  const double cs = get_col_scale(column);
  const double rs = get_row_scale(row);
  return raw * cs * rs;
}

bool LinearInterface::supports_set_coeff() const noexcept
{
  return m_backend_->supports_set_coeff();
}

// ── Simple delegations ──

void LinearInterface::set_obj_coeff(const ColIndex index, const double value)
{
  ensure_backend();
  m_backend_->set_obj_coeff(static_cast<int>(index), value);
}

// ── Raw column bound setters (LP/solver units) ──

void LinearInterface::set_col_low_raw(const ColIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  m_backend_->set_col_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col_upp_raw(const ColIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  m_backend_->set_col_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col_raw(const ColIndex index, const double value)
{
  set_col_low_raw(index, value);
  set_col_upp_raw(index, value);
}

// ── Physical column bound setters (physical_value / col_scale → LP) ──

void LinearInterface::set_col_low(const ColIndex index,
                                  const double physical_value)
{
  const double scale = get_col_scale(index);
  set_col_low_raw(index, physical_value / scale);
}

void LinearInterface::set_col_upp(const ColIndex index,
                                  const double physical_value)
{
  const double scale = get_col_scale(index);
  set_col_upp_raw(index, physical_value / scale);
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
  m_backend_->set_row_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_row_upp_raw(const RowIndex index, const double value)
{
  ensure_backend();
  assert(m_backend_ != nullptr);
  m_backend_->set_row_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_rhs_raw(const RowIndex row, const double rhs)
{
  // Match the rest of the raw-mutation setters above: ensure the
  // backend is live before delegating.  Without this, a mutation
  // issued after `release_backend()` would null-deref `m_backend_`
  // (flagged by clang-analyzer-core.CallAndMessage).
  ensure_backend();
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

// ── Names & LP file output ──

void LinearInterface::push_names_to_solver() const
{
  const auto ncols = static_cast<size_t>(m_backend_->get_num_cols());

  std::vector<std::string> col_names(ncols);
  for (const auto [i, name] :
       enumerate<ColIndex>(m_col_index_to_name_ | std::views::take(ncols)))
  {
    col_names[i] = name;
  }

  const auto nrows = static_cast<size_t>(m_backend_->get_num_rows());
  std::vector<std::string> row_names(nrows);
  if (m_label_maker_.row_names_enabled()) {
    for (const auto [i, name] :
         enumerate<RowIndex>(m_row_index_to_name_ | std::views::take(nrows)))
    {
      row_names[i] = name;
    }
  }

  m_backend_->push_names(col_names, row_names);
}

auto LinearInterface::write_lp(const std::string& filename) const
    -> std::expected<void, Error>
{
  if (filename.empty()) {
    return {};
  }

  if (m_row_index_to_name_.empty()) {
    return std::unexpected(Error {
        .code = ErrorCode::InvalidInput,
        .message = std::format(
            "LP file '{}' not saved: row names are not available. "
            "Use --lp-debug to enable LP name generation for file output.",
            filename),
    });
  }

  push_names_to_solver();
  m_backend_->write_lp(filename.c_str());
  return {};
}

// ── Algorithm fallback ──

namespace
{

/// Return the next algorithm in the fallback cycle:
/// barrier → dual → primal → barrier.
/// For default_algo, the cycle starts as if it were barrier.
constexpr LPAlgo next_fallback_algo(LPAlgo current) noexcept
{
  switch (current) {
    case LPAlgo::barrier:
      return LPAlgo::dual;
    case LPAlgo::dual:
      return LPAlgo::primal;
    case LPAlgo::primal:
      return LPAlgo::barrier;
    case LPAlgo::default_algo:
    case LPAlgo::last_algo:
      return LPAlgo::dual;
  }
  return LPAlgo::dual;
}

}  // namespace

// ── Solve ──

std::expected<int, Error> LinearInterface::initial_solve(
    const SolverOptions& solver_options)
{
  using Clock = std::chrono::steady_clock;

  ++m_solver_stats_.initial_solve_calls;
  m_solver_stats_.total_ncols += get_numcols();
  m_solver_stats_.total_nrows += get_numrows();

  try {
    // Start from backend-optimal defaults, overlay user settings on top.
    auto effective = m_backend_->optimal_options();
    effective.overlay(solver_options);
    m_last_solver_options_ = effective;

    m_backend_->apply_options(effective);

    const auto log_mode = effective.log_mode.value_or(SolverLogMode::nolog);
    const auto log_level = log_mode != SolverLogMode::nolog
        ? std::max(effective.log_level, 1)
        : effective.log_level;

    auto timed_solve = [&]
    {
      const auto t0 = Clock::now();
      if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
        const LogFileGuard log_guard(*this, m_log_file_, log_level);
        m_backend_->initial_solve();
      } else {
        const HandlerGuard guard(*this, log_level);
        m_backend_->initial_solve();
      }
      m_solver_stats_.total_solve_time_s +=
          std::chrono::duration<double>(Clock::now() - t0).count();
    };

    timed_solve();

    if (!is_optimal() && effective.max_fallbacks > 0) {
      // Algorithm fallback cycle: try alternative algorithms
      auto fallback_opts = effective;
      auto current_algo = effective.algorithm;

      for (int attempt = 0; attempt < effective.max_fallbacks && !is_optimal();
           ++attempt)
      {
        const auto next_algo = next_fallback_algo(current_algo);
        spdlog::warn("{}: initial_solve non-optimal with {}, fallback to {}",
                     m_log_tag_.empty() ? get_prob_name() : m_log_tag_,
                     current_algo,
                     next_algo);

        fallback_opts.algorithm = next_algo;
        fallback_opts.presolve = false;
        // Reset aggressive scaling on fallback — aggressive scaling can
        // cause numerical difficulties (high kappa) that prevent the
        // primary algorithm from converging; the fallback algorithm may
        // succeed with the solver's default scaling strategy.
        if (fallback_opts.scaling == SolverScaling::aggressive) {
          fallback_opts.scaling = SolverScaling::automatic;
        }
        m_backend_->apply_options(fallback_opts);

        ++m_solver_stats_.fallback_solves;
        timed_solve();

        current_algo = next_algo;
      }
    }

    if (!is_optimal()) {
      // Classify infeasibility for the end-of-run stats report.  Query
      // the backend directly (not get_status()) so primal/dual are
      // distinguishable — get_status() collapses them into status 2.
      ++m_solver_stats_.infeasible_count;
      if (m_backend_->is_proven_primal_infeasible()) {
        ++m_solver_stats_.primal_infeasible;
      } else if (m_backend_->is_proven_dual_infeasible()) {
        ++m_solver_stats_.dual_infeasible;
      }
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "Solver returned non-optimal for problem: {} status: {}{}",
              get_prob_name(),
              get_status(),
              effective.max_fallbacks > 0 ? " (after algorithm fallback cycle)"
                                          : ""),
          .status = get_status(),
      });
    }

    if (const auto k = m_backend_->get_kappa(); k.has_value()) {
      m_solver_stats_.max_kappa = std::max(m_solver_stats_.max_kappa, *k);
    }

    const auto status = get_status();
    cache_and_release();
    return status;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message =
            std::format("Unexpected error in initial_solve: {}", e.what()),
    });
  }
}

std::expected<int, Error> LinearInterface::resolve(
    const SolverOptions& solver_options)
{
  using Clock = std::chrono::steady_clock;

  ensure_backend();

  ++m_solver_stats_.resolve_calls;
  m_solver_stats_.total_ncols += get_numcols();
  m_solver_stats_.total_nrows += get_numrows();

  try {
    // Start from backend-optimal defaults, overlay user settings on top.
    auto effective = m_backend_->optimal_options();
    effective.overlay(solver_options);
    m_last_solver_options_ = effective;

    m_backend_->apply_options(effective);

    const auto log_mode = effective.log_mode.value_or(SolverLogMode::nolog);
    const auto log_level = log_mode != SolverLogMode::nolog
        ? std::max(effective.log_level, 1)
        : effective.log_level;

    auto timed_solve = [&]
    {
      const auto t0 = Clock::now();
      if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
        const LogFileGuard log_guard(*this, m_log_file_, log_level);
        m_backend_->resolve();
      } else {
        const HandlerGuard guard(*this, log_level);
        m_backend_->resolve();
      }
      m_solver_stats_.total_solve_time_s +=
          std::chrono::duration<double>(Clock::now() - t0).count();
    };

    timed_solve();

    if (!is_optimal() && effective.max_fallbacks > 0) {
      // Algorithm fallback cycle: try alternative algorithms
      auto fallback_opts = effective;
      auto current_algo = effective.algorithm;

      for (int attempt = 0; attempt < effective.max_fallbacks && !is_optimal();
           ++attempt)
      {
        const auto next_algo = next_fallback_algo(current_algo);
        // Demoted to DEBUG: these fallback-cycle lines were printing 2–3
        // consecutive warn lines per infeasible LP (barrier→dual→primal).
        // Final outcome is covered by the single "elastic ok" / "installed
        // fcut" INFO line in sddp_forward_pass.cpp, so the fallback-step
        // chatter is noise in the main log.  Still available at debug
        // level for a deep post-mortem.
        SPDLOG_DEBUG("{}: resolve non-optimal with {}, fallback to {}",
                     m_log_tag_.empty() ? get_prob_name() : m_log_tag_,
                     current_algo,
                     next_algo);

        fallback_opts.algorithm = next_algo;
        fallback_opts.presolve = false;
        // Reset aggressive scaling on fallback — aggressive scaling can
        // cause numerical difficulties (high kappa) that prevent the
        // primary algorithm from converging; the fallback algorithm may
        // succeed with the solver's default scaling strategy.
        if (fallback_opts.scaling == SolverScaling::aggressive) {
          fallback_opts.scaling = SolverScaling::automatic;
        }
        m_backend_->apply_options(fallback_opts);

        ++m_solver_stats_.fallback_solves;
        timed_solve();

        current_algo = next_algo;
      }
    }

    if (!is_optimal()) {
      // Classify infeasibility for the end-of-run stats report.
      ++m_solver_stats_.infeasible_count;
      if (m_backend_->is_proven_primal_infeasible()) {
        ++m_solver_stats_.primal_infeasible;
      } else if (m_backend_->is_proven_dual_infeasible()) {
        ++m_solver_stats_.dual_infeasible;
      }
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "Solver returned non-optimal for problem: {} status: {}{}",
              get_prob_name(),
              get_status(),
              effective.max_fallbacks > 0 ? " (after algorithm fallback cycle)"
                                          : ""),
          .status = get_status(),
      });
    }

    if (const auto k = m_backend_->get_kappa(); k.has_value()) {
      m_solver_stats_.max_kappa = std::max(m_solver_stats_.max_kappa, *k);
    }

    const auto status = get_status();
    cache_and_release();
    return status;

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message = std::format("Unexpected error in resolve: {}", e.what()),
    });
  }
}

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
  if (static_cast<size_t>(row) < m_row_index_to_name_.size()) {
    diag.name = m_row_index_to_name_[row];
  }

  // Row bounds (raw LP units)
  const auto row_lb = std::span(m_backend_->row_lower(), get_numrows());
  const auto row_ub = std::span(m_backend_->row_upper(), get_numrows());
  diag.rhs_lb = row_lb[static_cast<size_t>(row)];
  diag.rhs_ub = row_ub[static_cast<size_t>(row)];

  // Scan all columns for non-zero coefficients in this row
  for (const auto col : iota_range<ColIndex>(0, ncols)) {
    const double v = get_coeff_raw(row, col);
    if (v == 0.0) {
      continue;
    }
    const double abs_v = std::abs(v);
    ++diag.num_nonzeros;

    const auto& col_name =
        (static_cast<size_t>(col) < m_col_index_to_name_.size())
        ? m_col_index_to_name_[col]
        : "";

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

double LinearInterface::get_obj_value() const
{
  if (m_backend_released_) {
    return m_cached_obj_value_;
  }
  return m_backend_->obj_value();
}

double LinearInterface::get_obj_value_physical() const
{
  return get_obj_value() * m_scale_objective_;
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
