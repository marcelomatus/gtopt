/**
 * @file      highs_solver_backend.cpp
 * @brief     HiGHS native solver backend implementation
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <format>
#include <stdexcept>
#include <thread>
#include <utility>

#include "highs_solver_backend.hpp"

#include <HConfig.h>
#include <Highs.h>
#include <gtopt/hardware_info.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

namespace
{

/// Create a Highs instance with all output suppressed.
/// The constructor itself does not print, but passModel() and run()
/// call logHeader() which prints the banner when output_flag is true.
/// We disable output immediately so that any subsequent passModel()
/// or run() call will not produce a banner.
auto make_quiet_highs() -> std::unique_ptr<Highs>
{
  auto highs = std::make_unique<Highs>();
  highs->setOptionValue("output_flag", false);
  highs->setOptionValue("log_to_console", false);
  return highs;
}

/// Apply a SolverOptions bundle onto a Highs instance.  Pure Highs-level
/// mutation — never touches backend member fields.  Factored out so both
/// apply_options() (live path) and clone() (replay onto fresh instance)
/// can reuse the exact same parameter wiring.
///
/// Note on SolverOptions::memory_emphasis: HiGHS has no direct equivalent
/// to CPLEX `CPX_PARAM_MEMORYEMPHASIS` or any documented memory-compression
/// setting, so the option is accepted but intentionally ignored here.  See
/// solver_options.hpp: "HiGHS: no direct equivalent (ignored)".
void apply_options_to_highs(Highs& highs, const SolverOptions& opts)
{
  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    highs.setOptionValue("dual_feasibility_tolerance", *oeps);
  }
  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    highs.setOptionValue("primal_feasibility_tolerance", *feps);
  }
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    highs.setOptionValue("time_limit", *tl);
  }
  if (opts.threads > 0) {
    highs.setOptionValue("threads", opts.threads);
  }

  highs.setOptionValue("presolve", opts.presolve ? "on" : "off");

  // HiGHS simplex_scale_strategy: 0=off, 1=forced, 4=default.
  if (opts.scaling.has_value()) {
    int strategy = 4;  // default
    switch (*opts.scaling) {
      case SolverScaling::none:
        strategy = 0;
        break;
      case SolverScaling::automatic:
        strategy = 4;
        break;
      case SolverScaling::aggressive:
        strategy = 1;
        break;
    }
    highs.setOptionValue("simplex_scale_strategy", strategy);
  }

  {
    switch (opts.algorithm) {
      case LPAlgo::default_algo:
        highs.setOptionValue("solver", "choose");
        break;
      case LPAlgo::primal:
        highs.setOptionValue("solver", "simplex");
        highs.setOptionValue("simplex_strategy", 4);  // primal
        break;
      case LPAlgo::dual:
        highs.setOptionValue("solver", "simplex");
        highs.setOptionValue("simplex_strategy", 1);  // dual
        break;
      case LPAlgo::barrier:
        highs.setOptionValue("solver", "ipm");
        if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
          highs.setOptionValue("ipm_optimality_tolerance", *beps);
        }
        if (!opts.crossover) {
          highs.setOptionValue("run_crossover", "off");
        }
        break;
      case LPAlgo::last_algo:
        break;
    }
  }

  // Never enable console output here — logging is managed by the
  // LogFileGuard / HandlerGuard RAII wrappers in LinearInterface, which
  // direct output to a log file when log_mode is enabled.
  highs.setOptionValue("output_flag", false);
  highs.setOptionValue("log_to_console", false);
}

/// Enable HiGHS file logging with the provided basename + level.  When
/// level==0 or filename is empty the Highs instance stays silent.
void apply_log_filename_to_highs(Highs& highs,
                                 const std::string& filename,
                                 int level)
{
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    highs.setOptionValue("log_file", log_path);
    highs.setOptionValue("output_flag", true);
    highs.setOptionValue("log_to_console", false);
  }
}

}  // namespace

HighsSolverBackend::HighsSolverBackend()
    : m_highs_(make_quiet_highs())
{
}

HighsSolverBackend::~HighsSolverBackend() = default;

std::string_view HighsSolverBackend::solver_name() const noexcept
{
  return "highs";
}

std::string HighsSolverBackend::solver_version() const
{
  return std::format("{}.{}.{}",
                     HIGHS_VERSION_MAJOR,
                     HIGHS_VERSION_MINOR,
                     HIGHS_VERSION_PATCH);
}

double HighsSolverBackend::plugin_infinity() noexcept
{
  return kHighsInf;
}

double HighsSolverBackend::infinity() const noexcept
{
  return plugin_infinity();
}

bool HighsSolverBackend::supports_mip() const noexcept
{
  return true;
}

void HighsSolverBackend::set_prob_name(const std::string& name)
{
  m_prep_.prob_name = name;
}

std::string HighsSolverBackend::get_prob_name() const
{
  return m_prep_.prob_name;
}

void HighsSolverBackend::load_problem(int ncols,
                                      int nrows,
                                      const int* matbeg,
                                      const int* matind,
                                      const double* matval,
                                      const double* collb,
                                      const double* colub,
                                      const double* obj,
                                      const double* rowlb,
                                      const double* rowub)
{
  m_highs_->clear();
  // clear() resets output_flag to true (HiGHS default).  Suppress
  // output again before passModel() which prints the startup banner.
  m_highs_->setOptionValue("output_flag", false);
  m_highs_->setOptionValue("log_to_console", false);

  if (ncols == 0 && nrows == 0) {
    return;  // Empty problem — nothing to load
  }

  // Build HighsLp from CSC (column-sparse) format
  HighsLp lp;
  lp.num_col_ = ncols;
  lp.num_row_ = nrows;
  lp.sense_ = ObjSense::kMinimize;

  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  lp.col_cost_.assign(obj, obj + ncols);
  lp.col_lower_.assign(collb, collb + ncols);
  lp.col_upper_.assign(colub, colub + ncols);
  lp.row_lower_.assign(rowlb, rowlb + nrows);
  lp.row_upper_.assign(rowub, rowub + nrows);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  // Normalize bounds: clamp any value beyond ±kHighsInf to avoid
  // HiGHS warnings ("bounds >= 1e20 treated as +Infinity").
  // This is a safety net for callers that did not set
  // LinearProblem::set_infinity() before building.
  auto clamp_inf = [](double v)
  {
    if (v >= kHighsInf) {
      return kHighsInf;
    }
    return v <= -kHighsInf ? -kHighsInf : v;
  };
  std::ranges::transform(lp.col_lower_, lp.col_lower_.begin(), clamp_inf);
  std::ranges::transform(lp.col_upper_, lp.col_upper_.begin(), clamp_inf);
  std::ranges::transform(lp.row_lower_, lp.row_lower_.begin(), clamp_inf);
  std::ranges::transform(lp.row_upper_, lp.row_upper_.begin(), clamp_inf);

  // CSC matrix
  lp.a_matrix_.format_ = MatrixFormat::kColwise;
  if (ncols > 0) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    lp.a_matrix_.start_.assign(matbeg, matbeg + ncols + 1);
    const auto nnz = lp.a_matrix_.start_.back();
    lp.a_matrix_.index_.assign(matind, matind + nnz);
    lp.a_matrix_.value_.assign(matval, matval + nnz);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  const auto status = m_highs_->passModel(std::move(lp));
  if (status == HighsStatus::kError) {
    m_load_failed_ = true;
  }
}

int HighsSolverBackend::get_num_cols() const
{
  return m_highs_->getNumCol();
}

int HighsSolverBackend::get_num_rows() const
{
  return m_highs_->getNumRow();
}

void HighsSolverBackend::add_col(double lb, double ub, double obj)
{
  m_highs_->addCol(obj, lb, ub, 0, nullptr, nullptr);
}

void HighsSolverBackend::add_cols(int num_cols,
                                  const int* colbeg,
                                  const int* colind,
                                  const double* colval,
                                  const double* collb,
                                  const double* colub,
                                  const double* colobj)
{
  if (num_cols == 0) {
    return;
  }
  // HiGHS's CSC bulk variable API: same shape as the LinearInterface
  // hands us (colbeg/colind/colval/collb/colub/colobj).  Single call
  // replaces a per-column add_col loop and the associated reallocations
  // of HiGHS's column metadata vectors.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int nnz = colbeg[num_cols];
  m_highs_->addCols(
      num_cols, colobj, collb, colub, nnz, colbeg, colind, colval);
}

void HighsSolverBackend::set_col_lower(int index, double value)
{
  const auto& lp = m_highs_->getLp();
  const auto idx = static_cast<size_t>(index);
  const double current_upper =
      idx < lp.col_upper_.size() ? lp.col_upper_[idx] : kHighsInf;
  m_highs_->changeColBounds(index, value, current_upper);
}

void HighsSolverBackend::set_col_upper(int index, double value)
{
  const auto& lp = m_highs_->getLp();
  const auto idx = static_cast<size_t>(index);
  const double current_lower =
      idx < lp.col_lower_.size() ? lp.col_lower_[idx] : 0.0;
  m_highs_->changeColBounds(index, current_lower, value);
}

void HighsSolverBackend::set_obj_coeff(int index, double value)
{
  m_highs_->changeColCost(index, value);
}

void HighsSolverBackend::set_obj_coeffs(const double* values, int num_cols)
{
  // `changeColsCost(from_col, to_col, values)` is range-based; one call
  // updates [0, num_cols) verbatim.  No index array allocation needed
  // (unlike CPLEX).
  if (num_cols <= 0) {
    return;
  }
  m_highs_->changeColsCost(0, num_cols - 1, values);
}

void HighsSolverBackend::add_row(int num_elements,
                                 const int* columns,
                                 const double* elements,
                                 double rowlb,
                                 double rowub)
{
  m_highs_->addRow(rowlb, rowub, num_elements, columns, elements);
}

void HighsSolverBackend::add_rows(int num_rows,
                                  const int* rowbeg,
                                  const int* rowind,
                                  const double* rowval,
                                  const double* rowlb,
                                  const double* rowub)
{
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int nnz = rowbeg[num_rows];
  m_highs_->addRows(num_rows, rowlb, rowub, nnz, rowbeg, rowind, rowval);
}

void HighsSolverBackend::set_row_lower(int index, double value)
{
  const auto& lp = m_highs_->getLp();
  const auto idx = static_cast<size_t>(index);
  const double current_upper =
      idx < lp.row_upper_.size() ? lp.row_upper_[idx] : kHighsInf;
  m_highs_->changeRowBounds(index, value, current_upper);
}

void HighsSolverBackend::set_row_upper(int index, double value)
{
  const auto& lp = m_highs_->getLp();
  const auto idx = static_cast<size_t>(index);
  const double current_lower =
      idx < lp.row_lower_.size() ? lp.row_lower_[idx] : -kHighsInf;
  m_highs_->changeRowBounds(index, current_lower, value);
}

void HighsSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  m_highs_->changeRowBounds(index, lb, ub);
}

void HighsSolverBackend::delete_rows(int num, const int* indices)
{
  // HiGHS deleteRows takes a set of indices
  m_highs_->deleteRows(num, indices);
}

double HighsSolverBackend::get_coeff(int row, int col) const
{
  double value = 0;
  m_highs_->getCoeff(row, col, value);
  return value;
}

void HighsSolverBackend::set_coeff(int row, int col, double value)
{
  m_highs_->changeCoeff(row, col, value);
}

bool HighsSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

void HighsSolverBackend::set_continuous(int index)
{
  m_highs_->changeColIntegrality(index, HighsVarType::kContinuous);
}

void HighsSolverBackend::set_integer(int index)
{
  m_highs_->changeColIntegrality(index, HighsVarType::kInteger);
}

bool HighsSolverBackend::is_continuous(int index) const
{
  HighsVarType type {};
  // HiGHS doesn't have a direct "is continuous" check;
  // query integrality status
  const auto& lp = m_highs_->getLp();
  if (std::cmp_less(index, lp.integrality_.size())) {
    type = lp.integrality_[static_cast<size_t>(index)];
  }
  return type == HighsVarType::kContinuous;
}

bool HighsSolverBackend::is_integer(int index) const
{
  return !is_continuous(index);
}

void HighsSolverBackend::fetch_solution() const
{
  // Always re-fetch from HiGHS into the storage vectors.  No
  // backend-side validity flag: the LinearInterface
  // (`populate_solution_cache_post_solve`) is the single source of
  // truth post-solve and calls each accessor at most once per solve.
  // Storage is retained so the returned raw pointer remains valid
  // through the LI's `assign(...)` copy.
  const auto& solution = m_highs_->getSolution();
  m_col_solution_ = solution.col_value;
  m_col_dual_ = solution.col_dual;
  m_row_dual_ = solution.row_dual;
}

const double* HighsSolverBackend::col_lower() const
{
  return m_highs_->getLp().col_lower_.data();
}

const double* HighsSolverBackend::col_upper() const
{
  return m_highs_->getLp().col_upper_.data();
}

const double* HighsSolverBackend::obj_coefficients() const
{
  return m_highs_->getLp().col_cost_.data();
}

const double* HighsSolverBackend::row_lower() const
{
  return m_highs_->getLp().row_lower_.data();
}

const double* HighsSolverBackend::row_upper() const
{
  return m_highs_->getLp().row_upper_.data();
}

const double* HighsSolverBackend::col_solution() const
{
  fetch_solution();
  return m_col_solution_.data();
}

const double* HighsSolverBackend::reduced_cost() const
{
  fetch_solution();
  return m_col_dual_.data();
}

const double* HighsSolverBackend::row_price() const
{
  fetch_solution();
  return m_row_dual_.data();
}

double HighsSolverBackend::obj_value() const
{
  return m_highs_->getObjectiveValue();
}

void HighsSolverBackend::set_col_solution(const double* sol)
{
  if (sol == nullptr) {
    return;
  }
  HighsSolution solution;
  const auto ncols = m_highs_->getNumCol();
  solution.col_value.assign(
      sol,
      sol + ncols);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  solution.value_valid = true;
  m_highs_->setSolution(solution);
}

void HighsSolverBackend::set_row_price(const double* price)
{
  if (price == nullptr) {
    return;
  }
  // HiGHS can accept dual values as part of the solution
  HighsSolution solution;
  const auto nrows = m_highs_->getNumRow();
  solution.row_dual.assign(
      price,
      price
          + nrows);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  solution.dual_valid = true;
  m_highs_->setSolution(solution);
}

void HighsSolverBackend::initial_solve()
{
  if (m_load_failed_) {
    return;
  }
  const auto status = m_highs_->run();
  if (status == HighsStatus::kError) {
    throw std::runtime_error("HiGHS: solver error during initial_solve");
  }
}

void HighsSolverBackend::resolve()
{
  // HiGHS automatically warm-starts from previous basis
  if (m_load_failed_) {
    // passModel failed (e.g. contradictory bounds) — skip run so
    // is_proven_optimal() returns false and the caller reports
    // infeasibility.
    return;
  }
  const auto status = m_highs_->run();
  if (status == HighsStatus::kError) {
    throw std::runtime_error("HiGHS: solver error during resolve");
  }
}

void HighsSolverBackend::engage_robust_solve()
{
  if (m_highs_ == nullptr) {
    return;
  }

  // Helper: query a HiGHS double option, falling back to the default
  // if HiGHS reports an error (older versions may rename the option).
  auto get_double = [&](const std::string& key, double fallback) -> double
  {
    double v = fallback;
    if (m_highs_->getOptionValue(key, v) != HighsStatus::kOk) {
      return fallback;
    }
    return v;
  };

  if (!m_saved_robust_state_.has_value()) {
    RobustState saved {};
    saved.primal_feasibility_tolerance =
        get_double("primal_feasibility_tolerance", 1e-7);
    saved.dual_feasibility_tolerance =
        get_double("dual_feasibility_tolerance", 1e-7);
    saved.ipm_optimality_tolerance =
        get_double("ipm_optimality_tolerance", 1e-8);
    {
      std::string presolve_val = "on";
      m_highs_->getOptionValue("presolve", presolve_val);
      saved.presolve = presolve_val;
    }
    {
      int strategy = 4;
      m_highs_->getOptionValue("simplex_scale_strategy", strategy);
      saved.simplex_scale_strategy = strategy;
    }
    saved.engage_count = 0;
    m_saved_robust_state_ = saved;
  }
  ++m_saved_robust_state_->engage_count;

  // Compound the loosening — read live values, multiply by 10, clamp to
  // a sensible upper bound (HiGHS rejects tolerances above 1e-1).
  const double cur_pft =
      get_double("primal_feasibility_tolerance",
                 m_saved_robust_state_->primal_feasibility_tolerance);
  const double cur_dft =
      get_double("dual_feasibility_tolerance",
                 m_saved_robust_state_->dual_feasibility_tolerance);
  const double cur_ipm =
      get_double("ipm_optimality_tolerance",
                 m_saved_robust_state_->ipm_optimality_tolerance);

  constexpr double k_max_tol = 1e-1;
  m_highs_->setOptionValue("primal_feasibility_tolerance",
                           std::min(cur_pft * 10.0, k_max_tol));
  m_highs_->setOptionValue("dual_feasibility_tolerance",
                           std::min(cur_dft * 10.0, k_max_tol));
  m_highs_->setOptionValue("ipm_optimality_tolerance",
                           std::min(cur_ipm * 10.0, k_max_tol));

  // Force aggressive presolve (HiGHS has no separate "more passes" knob)
  // and force simplex_scale_strategy=4 (HiGHS' "forced" scaling, the
  // closest analogue to CPLEX NUMERICALEMPHASIS for poorly-scaled LPs).
  m_highs_->setOptionValue("presolve", std::string {"on"});
  m_highs_->setOptionValue("simplex_scale_strategy", 4);
}

void HighsSolverBackend::disengage_robust_solve() noexcept
{
  if (!m_saved_robust_state_.has_value()) {
    return;
  }
  if (m_highs_ == nullptr) {
    m_saved_robust_state_.reset();
    return;
  }

  const auto& s = *m_saved_robust_state_;
  // setOptionValue may return an error on bad input but cannot throw,
  // so the noexcept contract is fine.
  m_highs_->setOptionValue("primal_feasibility_tolerance",
                           s.primal_feasibility_tolerance);
  m_highs_->setOptionValue("dual_feasibility_tolerance",
                           s.dual_feasibility_tolerance);
  m_highs_->setOptionValue("ipm_optimality_tolerance",
                           s.ipm_optimality_tolerance);
  m_highs_->setOptionValue("presolve", s.presolve);
  m_highs_->setOptionValue("simplex_scale_strategy", s.simplex_scale_strategy);
  m_saved_robust_state_.reset();
}

bool HighsSolverBackend::is_proven_optimal() const
{
  return m_highs_->getModelStatus() == HighsModelStatus::kOptimal;
}

bool HighsSolverBackend::is_abandoned() const
{
  const auto status = m_highs_->getModelStatus();
  return status == HighsModelStatus::kSolveError
      || status == HighsModelStatus::kUnknown;
}

bool HighsSolverBackend::is_proven_primal_infeasible() const
{
  return m_highs_->getModelStatus() == HighsModelStatus::kInfeasible;
}

bool HighsSolverBackend::is_proven_dual_infeasible() const
{
  return m_highs_->getModelStatus() == HighsModelStatus::kUnbounded;
}

LPAlgo HighsSolverBackend::get_algorithm() const
{
  return m_algorithm_;
}

int HighsSolverBackend::get_threads() const
{
  return m_threads_;
}

bool HighsSolverBackend::get_presolve() const
{
  return m_presolve_;
}

int HighsSolverBackend::get_log_level() const
{
  return m_log_level_;
}

SolverOptions HighsSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::default_algo,
      .threads = 4,
      .presolve = true,
      .scaling = SolverScaling::automatic,
      .max_fallbacks = 2,
  };
}

void HighsSolverBackend::apply_options(const SolverOptions& opts)
{
  m_prep_.options = opts;
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;

  // HiGHS uses a *thread-local* task executor (despite the name
  // "resetGlobalScheduler").  Each OS thread that calls run() gets its
  // own executor with (threads-1) worker threads, auto-initialized on
  // first run().  Once initialized, changing the thread count requires
  // resetting the executor first — otherwise run() returns kError.
  //
  // This lives outside apply_options_to_highs() because it mutates
  // thread_local state tied to the *caller's* OS thread, not the Highs
  // instance.  The helper is side-effect-free w.r.t. thread_local state
  // so clone() on a different thread does not disturb the scheduler.
  const int effective = opts.threads > 0
      ? opts.threads
      : std::max(1, static_cast<int>(physical_concurrency()));
  static thread_local int tl_scheduler_threads {0};
  if (tl_scheduler_threads != 0 && tl_scheduler_threads != effective) {
    Highs::resetGlobalScheduler(/*blocking=*/true);
    tl_scheduler_threads = 0;
  }
  if (tl_scheduler_threads == 0) {
    tl_scheduler_threads = effective;
  }

  apply_options_to_highs(*m_highs_, opts);
}

std::optional<double> HighsSolverBackend::get_kappa() const
{
  double kappa = 0.0;
  // Highs::getKappa(exact=true) computes the exact condition number of
  // the basis matrix via forward/backward solve.  When HiGHS has no
  // basis (e.g. IPM without crossover) it returns kWarning/kError; we
  // propagate that as nullopt so callers never confuse "unavailable"
  // with a legitimate kappa of 1.0.
  if (m_highs_->getKappa(kappa, /*exact=*/true) != HighsStatus::kOk) {
    return std::nullopt;
  }
  return kappa;
}

void HighsSolverBackend::open_log(FILE* /*file*/, int level)
{
  // Enable solver output only when requested, but never to the console.
  // All solver output is directed to a log file via set_log_filename().
  m_highs_->setOptionValue("output_flag", level > 0);
  m_highs_->setOptionValue("log_to_console", false);
}

void HighsSolverBackend::close_log()
{
  m_highs_->setOptionValue("output_flag", false);
}

void HighsSolverBackend::set_log_filename(const std::string& filename,
                                          int level)
{
  if (level > 0 && !filename.empty()) {
    m_prep_.log_filename = filename;
    m_prep_.log_level = level;
    apply_log_filename_to_highs(*m_highs_, filename, level);
  }
}

void HighsSolverBackend::clear_log_filename()
{
  m_prep_.log_filename.clear();
  m_prep_.log_level = 0;
  m_highs_->setOptionValue("log_file", std::string {});
  m_highs_->setOptionValue("output_flag", false);
}

void HighsSolverBackend::push_names(const std::vector<std::string>& col_names,
                                    const std::vector<std::string>& row_names)
{
  for (int i = 0; std::cmp_less(i, col_names.size()); ++i) {
    if (!col_names[static_cast<size_t>(i)].empty()) {
      m_highs_->passColName(i, col_names[static_cast<size_t>(i)]);
    }
  }
  for (int i = 0; std::cmp_less(i, row_names.size()); ++i) {
    if (!row_names[static_cast<size_t>(i)].empty()) {
      m_highs_->passRowName(i, row_names[static_cast<size_t>(i)]);
    }
  }
}

void HighsSolverBackend::write_lp(const char* filename)
{
  const auto file = std::format("{}.lp", filename);
  m_highs_->writeModel(file);
}

std::unique_ptr<SolverBackend> HighsSolverBackend::clone() const
{
  auto cloned = std::make_unique<HighsSolverBackend>();

  // Propagate the full preparation state (options, log filename, log level,
  // prob name) and the per-getter cached scalars.  The previous version
  // only copied m_prob_name_ and silently dropped applied SolverOptions,
  // so a clone could fall back to HiGHS defaults (e.g. algorithm=choose)
  // while get_algorithm() still returned the source's value — diverging
  // backend state from solver state.
  cloned->m_prep_ = m_prep_;
  cloned->m_algorithm_ = m_algorithm_;
  cloned->m_threads_ = m_threads_;
  cloned->m_presolve_ = m_presolve_;
  cloned->m_log_level_ = m_log_level_;

  // Suppress banner before passModel() (constructor's settings were
  // already applied by make_quiet_highs, but be explicit).
  cloned->m_highs_->setOptionValue("output_flag", false);
  cloned->m_highs_->setOptionValue("log_to_console", false);

  // Replay options + logging from the prep cache onto the fresh Highs
  // instance so the clone actually solves with the same settings as the
  // source.
  if (cloned->m_prep_.options.has_value()) {
    apply_options_to_highs(*cloned->m_highs_, *cloned->m_prep_.options);
  }
  apply_log_filename_to_highs(*cloned->m_highs_,
                              cloned->m_prep_.log_filename,
                              cloned->m_prep_.log_level);

  // Re-create the model in the clone
  const auto& lp = m_highs_->getLp();
  cloned->m_highs_->passModel(lp);

  // Transfer basis if available
  const auto& basis = m_highs_->getBasis();
  if (basis.valid) {
    cloned->m_highs_->setBasis(basis);
  }

  return cloned;
}

}  // namespace gtopt
