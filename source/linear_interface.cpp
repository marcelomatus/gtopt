/**
 * @file      linear_interface.cpp
 * @brief     LinearInterface implementation — solver-agnostic via SolverBackend
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <expected>
#include <memory>
#include <ranges>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{
/// Check name uniqueness via map insertion (try_emplace pattern).
/// Returns true if the name is a duplicate.
inline bool check_name_unique(LinearInterface::name_index_map_t& name_map,
                              const std::string& name,
                              int32_t index,
                              std::string_view entity_type,
                              int level,
                              int min_level = 0)
{
  if (level < min_level || name.empty()) {
    return false;
  }

  auto [it, inserted] = name_map.try_emplace(name, index);
  if (!inserted) {
    if (level >= 2) {
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
    , m_log_file_(std::move(plog_file))
{
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

void LinearInterface::release_backend() noexcept
{
  if (m_low_memory_mode_ == LowMemoryMode::off) {
    m_backend_.reset();
    return;
  }

  if (m_backend_released_) {
    return;
  }

  // Cache the current solution for warm-start on reconstruction
  try {
    if (m_backend_ && is_optimal()) {
      const auto cs = get_col_sol_raw();
      const auto rd = get_row_dual_raw();
      m_snapshot_.cached_col_sol.assign(cs.begin(), cs.end());
      m_snapshot_.cached_row_dual.assign(rd.begin(), rd.end());
    }
    // Level 2: compress the snapshot
    enable_compression();
  } catch (...) {
    // Best-effort: proceed with release even if caching/compression fails
  }

  m_backend_.reset();
  m_backend_released_ = true;
}

// ── Low-memory mode ──

void LinearInterface::set_low_memory(LowMemoryMode mode,
                                     MemoryCodec codec) noexcept
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

void LinearInterface::capture_hot_start_cuts()
{
  if (m_low_memory_mode_ == LowMemoryMode::off) {
    return;
  }

  const auto base = static_cast<int>(m_base_numrows_);
  const auto current = static_cast<int>(get_numrows());
  const auto ncols = static_cast<int>(get_numcols());

  m_active_cuts_.clear();

  if (current > base) {
    const auto row_lo = get_row_low_raw();
    const auto row_hi = get_row_upp_raw();

    for (int r = base; r < current; ++r) {
      SparseRow row;
      row.lowb = row_lo[r];
      row.uppb = row_hi[r];
      for (int c = 0; c < ncols; ++c) {
        const auto val = get_coeff_raw(RowIndex {r}, ColIndex {c});
        if (val != 0.0) {
          row[ColIndex {c}] = val;
        }
      }
      m_active_cuts_.push_back(std::move(row));
    }
    SPDLOG_DEBUG("low_memory: captured {} hot-start cut rows", current - base);
  }

  m_backend_released_ = false;
}

void LinearInterface::reconstruct_backend(std::span<const double> col_sol,
                                          std::span<const double> row_dual)
{
  if (!m_backend_released_ || !m_snapshot_.has_data()) {
    return;
  }

  // Level 2: decompress the snapshot
  disable_compression();

  // 1. Reload the base structural LP
  load_flat(m_snapshot_.flat_lp);

  // 2. Replay dynamic columns (typically just alpha — very few)
  for (const auto& col : m_dynamic_cols_) {
    add_col(col);
  }

  // 3. Mark structural boundary
  save_base_numrows();

  // 4. Bulk-add active cuts (single efficient call)
  if (!m_active_cuts_.empty()) {
    add_rows(m_active_cuts_);
  }

  // 5. Load previous solution/duals for warm-start.
  //    Use explicit arguments if provided, otherwise fall back to cached.
  const auto& ws_col = col_sol.empty() ? m_snapshot_.cached_col_sol : col_sol;
  const auto& ws_dual =
      row_dual.empty() ? m_snapshot_.cached_row_dual : row_dual;
  if (!ws_col.empty() || !ws_dual.empty()) {
    set_warm_start_solution(ws_col, ws_dual);
  }

  m_backend_released_ = false;
}

void LinearInterface::record_dynamic_col(SparseCol col)
{
  if (m_low_memory_mode_ != LowMemoryMode::off) {
    m_dynamic_cols_.push_back(std::move(col));
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
  m_snapshot_.compress(m_memory_codec_);
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

void LinearInterface::set_log_file(const std::string& plog_file)
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

LinearInterface LinearInterface::clone(std::span<const double> col_sol,
                                       std::span<const double> row_dual) const
{
  auto cloned = LinearInterface {m_backend_->clone(), m_log_file_};
  cloned.m_scale_objective_ = m_scale_objective_;
  cloned.m_col_scales_ = m_col_scales_;
  cloned.m_variable_scale_map_ = m_variable_scale_map_;

  if (!col_sol.empty() || !row_dual.empty()) {
    cloned.set_warm_start_solution(col_sol, row_dual);
  }

  return cloned;
}

void LinearInterface::set_warm_start_solution(
    const std::span<const double> col_sol,
    const std::span<const double> row_dual)
{
  if (!col_sol.empty()) {
    const auto ncols = static_cast<std::size_t>(get_numcols());
    if (col_sol.size() >= ncols) {
      set_col_sol(col_sol.first(ncols));
    } else {
      std::vector<double> padded(ncols, 0.0);
      std::ranges::copy(col_sol, padded.begin());
      set_col_sol(padded);
    }
  }
  if (!row_dual.empty()) {
    const auto nrows = static_cast<std::size_t>(get_numrows());
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

  m_backend_->set_prob_name(flat_lp.name);

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

  // Preserve VariableScaleMap for dynamic column auto-scaling.
  m_variable_scale_map_ = flat_lp.variable_scale_map;

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

  // Build name maps
  auto build_name_map = []<typename IndexType>(const auto& names_vec,
                                               name_index_map_t& name_map,
                                               auto& index_to_name)
  {
    index_to_name.assign(names_vec.begin(), names_vec.end());
    name_map.reserve(names_vec.size());

    for (const auto [i, name] : enumerate<IndexType>(names_vec)) {
      if (!name.empty()) {
        name_map.emplace(name, static_cast<int>(i));
      }
    }
  };

  if (m_lp_names_level_ >= 0 && !flat_lp.colnm.empty()) {
    build_name_map.template operator()<ColIndex>(
        flat_lp.colnm, m_col_names_, m_col_index_to_name_);
  }

  if (m_lp_names_level_ >= 1 && !flat_lp.rownm.empty()) {
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
  const auto index = m_backend_->get_num_cols();
  check_name_unique(m_col_names_, name, index, "column", m_lp_names_level_, 0);

  m_backend_->add_col(normalize_bound(collb), normalize_bound(colub), 0.0);

  const auto col = ColIndex {index};
  if (m_lp_names_level_ >= 0 && !name.empty()) {
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

ColIndex LinearInterface::add_col(const SparseCol& col)
{
  // Resolve name: explicit name takes priority, then metadata-based generation.
  const auto name = [&]() -> std::string
  {
    if (!col.name.empty()) {
      return std::string(col.name);
    }
    if (!col.class_name.empty()
        && !std::holds_alternative<std::monostate>(col.context))
    {
      return std::visit(
          [&](const auto& ctx) -> std::string
          {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(ctx)>,
                                         std::monostate>)
            {
              return {};
            } else {
              return generate_lp_label(
                  col.class_name, col.variable_name, col.variable_uid, ctx);
            }
          },
          col.context);
    }
    return {};
  }();

  const auto [lowb, uppb] = normalize_bounds(col.lowb, col.uppb);

  const auto index = m_backend_->get_num_cols();
  check_name_unique(m_col_names_, name, index, "column", m_lp_names_level_, 0);

  m_backend_->add_col(lowb, uppb, col.cost);

  const auto col_idx = ColIndex {index};
  if (m_lp_names_level_ >= 0 && !name.empty()) {
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
  check_name_unique(m_row_names_, name, index, "row", m_lp_names_level_, 1);

  m_backend_->add_row(static_cast<int>(numberElements),
                      columns.data(),
                      elements.data(),
                      normalize_bound(rowlb),
                      normalize_bound(rowub));

  const auto row_idx = RowIndex {index};
  if (m_lp_names_level_ >= 1 && !name.empty()) {
    if (m_row_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_row_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_row_index_to_name_[row_idx] = name;
  }

  return row_idx;
}

RowIndex LinearInterface::add_row(const SparseRow& row, const double eps)
{
  // Resolve name: generate from metadata when available.
  const auto name = [&]() -> std::string
  {
    if (!row.class_name.empty()
        && !std::holds_alternative<std::monostate>(row.context))
    {
      return std::visit(
          [&](const auto& ctx) -> std::string
          {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(ctx)>,
                                         std::monostate>)
            {
              return {};
            } else {
              return generate_lp_label(
                  row.class_name, row.constraint_name, row.variable_uid, ctx);
            }
          },
          row.context);
    }
    return {};
  }();

  const auto rs = row.scale;

  if (rs != 1.0) {
    // Scale row: divide bounds and coefficients by row.scale.
    // This mirrors how flatten() handles SparseRow::scale for static rows.
    const auto inv_rs = 1.0 / rs;
    const auto infy = m_backend_->infinity();

    // Scale coefficients: build a new flat representation directly
    // from the original row, applying the scale factor.
    auto [columns, elements] = row.to_flat<int>(eps);
    for (auto& v : elements) {
      v *= inv_rs;
    }

    // Scale bounds (preserve infinity)
    const double lb =
        (row.lowb > -infy && row.lowb < infy) ? row.lowb * inv_rs : row.lowb;
    const double ub =
        (row.uppb > -infy && row.uppb < infy) ? row.uppb * inv_rs : row.uppb;

    const auto row_idx =
        add_row(name, columns.size(), columns, elements, lb, ub);

    // Register the row scale so physical getters/setters account for it.
    set_row_scale(row_idx, rs);

    return row_idx;
  }

  const auto [columns, elements] = row.to_flat<int>(eps);
  return add_row(name, columns.size(), columns, elements, row.lowb, row.uppb);
}

void LinearInterface::add_rows(const std::span<const SparseRow> rows,
                               const double eps)
{
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
  for (const auto [r, row] : std::views::enumerate(rows)) {
    rowbeg[static_cast<size_t>(r)] = static_cast<int>(rowind.size());

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
    rowlb[static_cast<size_t>(r)] = normalize_bound(lb);
    rowub[static_cast<size_t>(r)] = normalize_bound(ub);
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
  for (const auto [r, row] : std::views::enumerate(rows)) {
    const auto row_idx = RowIndex {first_row_index + static_cast<int>(r)};

    if (row.scale != 1.0) {
      set_row_scale(row_idx, row.scale);
    }

    if (m_lp_names_level_ >= 1) {
      const auto name = [&]() -> std::string
      {
        if (!row.class_name.empty()
            && !std::holds_alternative<std::monostate>(row.context))
        {
          return std::visit(
              [&](const auto& ctx) -> std::string
              {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(ctx)>,
                                             std::monostate>)
                {
                  return {};
                } else {
                  return generate_lp_label(row.class_name,
                                           row.constraint_name,
                                           row.variable_uid,
                                           ctx);
                }
              },
              row.context);
        }
        return {};
      }();

      check_name_unique(m_row_names_,
                        name,
                        static_cast<int>(row_idx),
                        "row",
                        m_lp_names_level_,
                        1);

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
  if (indices.empty()) {
    return;
  }

  m_backend_->delete_rows(static_cast<int>(indices.size()), indices.data());

  if (m_lp_names_level_ >= 1) {
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
  if (m_lp_names_level_ >= 2) {
    m_row_names_.clear();
    m_row_names_.reserve(m_row_index_to_name_.size());
    for (const auto [i, name] : enumerate<RowIndex>(m_row_index_to_name_)) {
      if (!name.empty()) {
        m_row_names_.emplace(name, static_cast<int32_t>(i));
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

  if (m_lp_names_level_ >= 1) {
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
  m_backend_->set_obj_coeff(static_cast<int>(index), value);
}

// ── Raw column bound setters (LP/solver units) ──

void LinearInterface::set_col_low_raw(const ColIndex index, const double value)
{
  m_backend_->set_col_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col_upp_raw(const ColIndex index, const double value)
{
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
  m_backend_->set_row_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_row_upp_raw(const RowIndex index, const double value)
{
  m_backend_->set_row_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_rhs_raw(const RowIndex row, const double rhs)
{
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
  return static_cast<size_t>(m_backend_->get_num_rows());
}

size_t LinearInterface::get_numcols() const
{
  return static_cast<size_t>(m_backend_->get_num_cols());
}

void LinearInterface::set_continuous(const ColIndex index)
{
  m_backend_->set_continuous(static_cast<int>(index));
}

void LinearInterface::set_integer(const ColIndex index)
{
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
  if (m_lp_names_level_ >= 1) {
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
            "Set names_level >= only_cols to enable LP file output.",
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

    if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
      const LogFileGuard log_guard(*this, m_log_file_, log_level);
      m_backend_->initial_solve();
    } else {
      const HandlerGuard guard(*this, log_level);
      m_backend_->initial_solve();
    }

    if (!is_optimal() && effective.max_fallbacks > 0) {
      // Algorithm fallback cycle: try alternative algorithms
      auto fallback_opts = effective;
      auto current_algo = effective.algorithm;

      for (int attempt = 0; attempt < effective.max_fallbacks && !is_optimal();
           ++attempt)
      {
        const auto next_algo = next_fallback_algo(current_algo);
        spdlog::warn(
            "initial_solve: {} non-optimal with {}, "
            "fallback to {}",
            get_prob_name(),
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

        if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
          const LogFileGuard log_guard(*this, m_log_file_, log_level);
          m_backend_->initial_solve();
        } else {
          const HandlerGuard guard(*this, log_level);
          m_backend_->initial_solve();
        }

        current_algo = next_algo;
      }
    }

    if (!is_optimal()) {
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

    return get_status();

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

    if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
      const LogFileGuard log_guard(*this, m_log_file_, log_level);
      m_backend_->resolve();
    } else {
      const HandlerGuard guard(*this, log_level);
      m_backend_->resolve();
    }

    if (!is_optimal() && effective.max_fallbacks > 0) {
      // Algorithm fallback cycle: try alternative algorithms
      auto fallback_opts = effective;
      auto current_algo = effective.algorithm;

      for (int attempt = 0; attempt < effective.max_fallbacks && !is_optimal();
           ++attempt)
      {
        const auto next_algo = next_fallback_algo(current_algo);
        spdlog::warn(
            "resolve: {} non-optimal with {}, "
            "fallback to {}",
            get_prob_name(),
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

        if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
          const LogFileGuard log_guard(*this, m_log_file_, log_level);
          m_backend_->resolve();
        } else {
          const HandlerGuard guard(*this, log_level);
          m_backend_->resolve();
        }

        current_algo = next_algo;
      }
    }

    if (!is_optimal()) {
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

    return get_status();

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
  m_backend_->resolve();

  // Update cached options so subsequent dual accesses don't re-solve.
  m_last_solver_options_.crossover = true;

  SPDLOG_INFO("lazy crossover: computed duals on demand ({})", get_prob_name());
}

// ── Status ──

int LinearInterface::get_status() const
{
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

double LinearInterface::get_kappa() const
{
  return m_backend_->get_kappa();
}

RowDiagnostics LinearInterface::diagnose_row(const RowIndex row) const
{
  const auto ncols = static_cast<int>(get_numcols());
  const auto ri = static_cast<int>(row);

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
  diag.rhs_lb = row_lb[static_cast<size_t>(ri)];
  diag.rhs_ub = row_ub[static_cast<size_t>(ri)];

  // Scan all columns for non-zero coefficients in this row
  for (int c = 0; c < ncols; ++c) {
    const double v = m_backend_->get_coeff(ri, c);
    if (v == 0.0) {
      continue;
    }
    const double abs_v = std::abs(v);
    ++diag.num_nonzeros;

    const auto col = ColIndex {c};
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
  return m_backend_->is_proven_optimal();
}

bool LinearInterface::is_dual_infeasible() const
{
  return m_backend_->is_proven_dual_infeasible();
}

bool LinearInterface::is_prim_infeasible() const
{
  return m_backend_->is_proven_primal_infeasible();
}

double LinearInterface::get_obj_value() const
{
  return m_backend_->obj_value();
}

double LinearInterface::get_obj_value_physical() const
{
  return m_backend_->obj_value() * m_scale_objective_;
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
