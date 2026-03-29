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
#include <gtopt/solver_registry.hpp>
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
                                 const std::string& plog_file)
    : m_backend_(std::move(backend))
    , m_log_file_(plog_file)
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

LinearInterface LinearInterface::clone() const
{
  auto cloned = LinearInterface {m_backend_->clone(), m_log_file_};
  cloned.m_col_scales_ = m_col_scales_;
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

  // Preserve per-column scale factors from LinearProblem.
  m_col_scales_ = flat_lp.col_scales;

  // Preserve coefficient statistics computed during lp_build().
  m_stats_nnz_ = flat_lp.stats_nnz;
  m_stats_zeroed_ = flat_lp.stats_zeroed;
  m_stats_max_abs_ = flat_lp.stats_max_abs;
  m_stats_min_abs_ = flat_lp.stats_min_abs;
  m_stats_max_col_ = flat_lp.stats_max_col;
  m_stats_min_col_ = flat_lp.stats_min_col;
  m_stats_max_col_name_ = flat_lp.stats_max_col_name;
  m_stats_min_col_name_ = flat_lp.stats_min_col_name;

  for (auto i : flat_lp.colint) {
    m_backend_->set_integer(i);
  }

  // Build name maps
  auto build_name_map = [](const auto& names_vec,
                           name_index_map_t& name_map,
                           std::vector<std::string>& index_to_name)
  {
    index_to_name.resize(names_vec.size());
    name_map.reserve(names_vec.size());

    for (int i = 0; const auto& name : names_vec) {
      if (!name.empty()) {
        index_to_name[static_cast<size_t>(i)] = name;
        name_map.emplace(name, i);
      }
      ++i;
    }
  };

  if (m_lp_names_level_ >= 0 && !flat_lp.colnm.empty()) {
    build_name_map(flat_lp.colnm, m_col_names_, m_col_index_to_name_);
  }

  if (m_lp_names_level_ >= 1 && !flat_lp.rownm.empty()) {
    build_name_map(flat_lp.rownm, m_row_names_, m_row_index_to_name_);
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

  if (m_lp_names_level_ >= 0 && !name.empty()) {
    if (m_col_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_col_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_col_index_to_name_[static_cast<size_t>(index)] = name;
  }

  return ColIndex {index};
}

ColIndex LinearInterface::add_col(const std::string& name)
{
  return add_col(name, 0.0, m_backend_->infinity());
}

ColIndex LinearInterface::add_free_col(const std::string& name)
{
  return add_col(name, -m_backend_->infinity(), m_backend_->infinity());
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

  if (m_lp_names_level_ >= 1 && !name.empty()) {
    if (m_row_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_row_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_row_index_to_name_[static_cast<size_t>(index)] = name;
  }

  return RowIndex {index};
}

RowIndex LinearInterface::add_row(const SparseRow& row, const double eps)
{
  const auto [columns, elements] = row.to_flat<int>(eps);

  return add_row(
      row.name, columns.size(), columns, elements, row.lowb, row.uppb);
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
    for (int32_t i = 0; const auto& name : m_row_index_to_name_) {
      if (!name.empty()) {
        m_row_names_.emplace(name, i);
      }
      ++i;
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
  const auto src_col_lo = source.get_col_low();
  const auto src_col_hi = source.get_col_upp();
  for (int c = 0; c < ncols; ++c) {
    m_backend_->set_col_lower(c, src_col_lo[c]);
    m_backend_->set_col_upper(c, src_col_hi[c]);
  }

  const auto nrows = m_backend_->get_num_rows();
  const auto src_row_lo = source.get_row_low();
  const auto src_row_hi = source.get_row_upp();
  for (int r = 0; r < nrows; ++r) {
    m_backend_->set_row_lower(r, src_row_lo[r]);
    m_backend_->set_row_upper(r, src_row_hi[r]);
  }

  if (m_lp_names_level_ >= 1) {
    m_row_index_to_name_.resize(static_cast<size_t>(nrows));
    rebuild_row_name_maps();
  }
}

// ── Coefficients ──

void LinearInterface::set_coeff(const RowIndex row,
                                const ColIndex column,
                                const double value)
{
  m_backend_->set_coeff(static_cast<int>(row), static_cast<int>(column), value);
}

double LinearInterface::get_coeff(const RowIndex row,
                                  const ColIndex column) const
{
  return m_backend_->get_coeff(static_cast<int>(row), static_cast<int>(column));
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

void LinearInterface::set_col_low(const ColIndex index, const double value)
{
  m_backend_->set_col_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col_upp(const ColIndex index, const double value)
{
  m_backend_->set_col_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_row_low(const RowIndex index, const double value)
{
  m_backend_->set_row_lower(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_row_upp(const RowIndex index, const double value)
{
  m_backend_->set_row_upper(static_cast<int>(index), normalize_bound(value));
}

void LinearInterface::set_col(const ColIndex index, const double value)
{
  set_col_low(index, value);
  set_col_upp(index, value);
}

void LinearInterface::set_rhs(const RowIndex row, const double rhs)
{
  m_backend_->set_row_bounds(static_cast<int>(row), rhs, rhs);
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
  set_col_low(index, 0);
  set_col_upp(index, 1);
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
  for (size_t i = 0; i < std::min(m_col_index_to_name_.size(), ncols); ++i) {
    col_names[i] = m_col_index_to_name_[i];
  }

  const auto nrows = static_cast<size_t>(m_backend_->get_num_rows());
  std::vector<std::string> row_names(nrows);
  if (m_lp_names_level_ >= 1) {
    for (size_t i = 0; i < std::min(m_row_index_to_name_.size(), nrows); ++i) {
      row_names[i] = m_row_index_to_name_[i];
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

// ── Solve ──

std::expected<int, Error> LinearInterface::initial_solve(
    const SolverOptions& solver_options)
{
  try {
    m_backend_->apply_options(solver_options);

    const auto log_mode =
        solver_options.log_mode.value_or(SolverLogMode::nolog);
    const auto log_level = log_mode != SolverLogMode::nolog
        ? std::max(solver_options.log_level, 1)
        : solver_options.log_level;

    if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
      // Use native file-based logging (set_log_filename API)
      const LogFileGuard log_guard(*this, m_log_file_, log_level);
      m_backend_->initial_solve();
    } else {
      // Use legacy FILE*-based logging (open_log API)
      const HandlerGuard guard(*this, log_level);
      m_backend_->initial_solve();
    }

    if (!is_optimal()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "Solver returned non-optimal for problem: {} status: {}",
              get_prob_name(),
              get_status()),
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
    m_backend_->apply_options(solver_options);

    const auto log_mode =
        solver_options.log_mode.value_or(SolverLogMode::nolog);
    const auto log_level = log_mode != SolverLogMode::nolog
        ? std::max(solver_options.log_level, 1)
        : solver_options.log_level;

    if (log_mode != SolverLogMode::nolog && !m_log_file_.empty()) {
      // Use native file-based logging (set_log_filename API)
      const LogFileGuard log_guard(*this, m_log_file_, log_level);
      m_backend_->resolve();
    } else {
      // Use legacy FILE*-based logging (open_log API)
      const HandlerGuard guard(*this, log_level);
      m_backend_->resolve();
    }

    if (!is_optimal()) {
      return std::unexpected(Error {
          .code = ErrorCode::SolverError,
          .message = std::format(
              "Solver returned non-optimal for problem: {} status: {}",
              get_prob_name(),
              get_status()),
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
