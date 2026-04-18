/**
 * @file      mindopt_solver_backend.cpp
 * @brief     MindOpt C API solver backend implementation
 * @date      Fri Apr  4 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <vector>

#include "mindopt_solver_backend.hpp"

#include <Mindopt.h>
#include <fcntl.h>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>
#include <unistd.h>

namespace gtopt
{

namespace
{

/// Apply every SolverOptions field onto a *fresh* MindOpt env.
///
/// Pure helper: mutates `env` only, touches no backend members.  Shared
/// between the live `apply_options()` path and the clone path, so any
/// option the caller ever set is replayed onto the new env on clone().
///
/// NOTE: SolverOptions::low_memory has no documented MindOpt C API
/// equivalent.  We deliberately leave it as a no-op here rather than
/// forcing a proxy (e.g. single-threaded or simplex-only) that would
/// slow down all solves.
void apply_options_to_env(MDOenv* env, const SolverOptions& opts)
{
  if (env == nullptr) {
    return;
  }

  if (opts.threads > 0) {
    MDOsetintparam(env, MDO_INT_PAR_NUM_THREADS, opts.threads);
  }

  MDOsetintparam(env, MDO_INT_PAR_PRESOLVE, opts.presolve ? 1 : 0);

  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    MDOsetdblparam(env, MDO_DBL_PAR_SPX_DUAL_TOLERANCE, *oeps);
  }
  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    MDOsetdblparam(env, MDO_DBL_PAR_SPX_PRIMAL_TOLERANCE, *feps);
  }
  if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
    MDOsetdblparam(env, MDO_DBL_PAR_IPM_GAP_TOLERANCE, *beps);
  }
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    MDOsetdblparam(env, MDO_DBL_PAR_MAX_TIME, *tl);
  }

  // Method: -1=auto, 0=primal simplex, 1=dual simplex, 2=barrier, 3=concurrent
  switch (opts.algorithm) {
    case LPAlgo::default_algo:
      MDOsetintparam(env, MDO_INT_PAR_METHOD, -1);  // auto
      break;
    case LPAlgo::primal:
      MDOsetintparam(env, MDO_INT_PAR_METHOD, 0);  // primal simplex
      break;
    case LPAlgo::dual:
      MDOsetintparam(env, MDO_INT_PAR_METHOD, 1);  // dual simplex
      break;
    case LPAlgo::barrier:
      MDOsetintparam(env, MDO_INT_PAR_METHOD, 2);  // barrier/IPM
      MDOsetintparam(env, MDO_INT_PAR_SOLUTION_TARGET, opts.crossover ? 0 : 2);
      break;
    case LPAlgo::last_algo:
      break;
  }

  MDOsetintparam(env, MDO_INT_PAR_OUTPUT_FLAG, opts.log_level > 0 ? 1 : 0);
}

/// Apply cached log-filename settings to a fresh env.  level<=0 or empty
/// filename leaves the env in its silent default.
void apply_log_filename_to_env(MDOenv* env,
                               const std::string& filename,
                               int level)
{
  if (env == nullptr || level <= 0 || filename.empty()) {
    return;
  }
  const auto log_path = std::format("{}.log", filename);
  MDOsetstrparam(env, MDO_STR_PAR_LOG_FILE, log_path.c_str());
  MDOsetintparam(env, MDO_INT_PAR_OUTPUT_FLAG, 1);
}

}  // namespace

// ── helpers ──────────────────────────────────────────────────────────────

void MindOptSolverBackend::check_error(int rc, const char* func)
{
  if (rc != MDO_OKAY) {
    const char* msg = MDOexplainerror(rc);
    throw std::runtime_error(std::format(
        "MindOpt: {} failed (rc={}: {})", func, rc, msg != nullptr ? msg : ""));
  }
}

void MindOptSolverBackend::reset_model_()
{
  if (m_model_ != nullptr) {
    MDOfreemodel(m_model_);
    m_model_ = nullptr;
  }
  const int rc = MDOnewmodel(
      m_env_,
      &m_model_,
      m_prep_.prob_name.empty() ? "gtopt" : m_prep_.prob_name.c_str(),
      0,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  check_error(rc, "MDOnewmodel");
  MDOsetintattr(m_model_, MDO_INT_ATTR_MODEL_SENSE, MDO_MINIMIZE);
  m_prob_cached_ = false;
  m_sol_cached_ = false;
}

// ── ctor / dtor ──────────────────────────────────────────────────────────

MindOptSolverBackend::MindOptSolverBackend()
{
  // Redirect stdout to /dev/null before any MindOpt calls.
  // MindOpt prints a banner to stdout that cannot be suppressed via API.
  const int saved_stdout = ::dup(STDOUT_FILENO);  // NOLINT(android-cloexec-dup)
  const int devnull = ::open("/dev/null", O_WRONLY);  // NOLINT
  if (devnull >= 0) {
    ::dup2(devnull, STDOUT_FILENO);
    ::close(devnull);
  }

  int rc = MDOemptyenv(&m_env_);
  if (rc != MDO_OKAY) {
    // Restore stdout before throwing
    if (saved_stdout >= 0) {
      ::dup2(saved_stdout, STDOUT_FILENO);
      ::close(saved_stdout);
    }
    throw std::runtime_error(
        std::format("MindOpt: MDOemptyenv failed (rc={})", rc));
  }

  MDOsetintparam(m_env_, MDO_INT_PAR_OUTPUT_FLAG, 0);
  MDOsetintparam(m_env_, MDO_INT_PAR_LOG_TO_CONSOLE, 0);

  rc = MDOstartenv(m_env_);

  // Restore stdout
  if (saved_stdout >= 0) {
    ::dup2(saved_stdout, STDOUT_FILENO);
    ::close(saved_stdout);
  }
  if (rc != MDO_OKAY) {
    const char* msg = MDOexplainerror(rc);
    MDOfreeenv(m_env_);
    m_env_ = nullptr;
    throw std::runtime_error(
        std::format("MindOpt: MDOstartenv failed (rc={}: {}). "
                    "Set MINDOPT_HOME to your MindOpt installation directory "
                    "and ensure a valid license file (mindopt.lic) is present.",
                    rc,
                    msg != nullptr ? msg : "unknown error"));
  }

  rc = MDOnewmodel(m_env_,
                   &m_model_,
                   "gtopt",
                   0,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr);
  check_error(rc, "MDOnewmodel");

  // Set minimization
  MDOsetintattr(m_model_, MDO_INT_ATTR_MODEL_SENSE, MDO_MINIMIZE);
}

MindOptSolverBackend::~MindOptSolverBackend()
{
  if (m_model_ != nullptr) {
    MDOfreemodel(m_model_);
  }
  if (m_env_ != nullptr) {
    MDOfreeenv(m_env_);
  }
}

// ── identity ─────────────────────────────────────────────────────────────

std::string_view MindOptSolverBackend::solver_name() const noexcept
{
  return "mindopt";
}

std::string MindOptSolverBackend::solver_version() const
{
  int major = 0;
  int minor = 0;
  int technical = 0;
  MDOversion(&major, &minor, &technical);
  return std::format("{}.{}.{}", major, minor, technical);
}

double MindOptSolverBackend::infinity() const noexcept
{
  return MDO_INFINITY;
}

bool MindOptSolverBackend::supports_mip() const noexcept
{
  return true;
}

// ── problem name ─────────────────────────────────────────────────────────

void MindOptSolverBackend::set_prob_name(const std::string& name)
{
  m_prep_.prob_name = name;
  MDOsetstrattr(m_model_, MDO_STR_ATTR_MODEL_NAME, name.c_str());
}

std::string MindOptSolverBackend::get_prob_name() const
{
  char* name = nullptr;
  if (MDOgetstrattr(m_model_, MDO_STR_ATTR_MODEL_NAME, &name) == MDO_OKAY
      && name != nullptr)
  {
    return {name};
  }
  return {};
}

// ── bulk load ────────────────────────────────────────────────────────────

void MindOptSolverBackend::load_problem(int ncols,
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
  // Drop the previous model and create a fresh one replaying cached prob_name.
  reset_model_();

  if (ncols == 0 && nrows == 0) {
    return;
  }

  // MindOpt MDOloadmodel wants sense/rhs per row.  We need to convert
  // from ranged bounds (rowlb, rowub) to sense + rhs.
  // Strategy: drop the empty model and recreate it with columns preloaded.

  if (m_model_ != nullptr) {
    MDOfreemodel(m_model_);
    m_model_ = nullptr;
  }

  // Step 1: create model with columns only (no constraints)
  int rc = MDOnewmodel(
      m_env_,
      &m_model_,
      m_prep_.prob_name.empty() ? "gtopt" : m_prep_.prob_name.c_str(),
      ncols,
      const_cast<double*>(obj),  // NOLINT
      const_cast<double*>(collb),  // NOLINT
      const_cast<double*>(colub),  // NOLINT
      nullptr,  // vtype: all continuous
      nullptr);  // varnames
  check_error(rc, "MDOnewmodel");

  MDOsetintattr(m_model_, MDO_INT_ATTR_MODEL_SENSE, MDO_MINIMIZE);

  // Step 2: convert CSC to CSR and add range constraints in batch
  // Build CSR from CSC
  const bool have_nnz = (ncols > 0 && matbeg != nullptr);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto nnz = have_nnz ? matbeg[ncols] : 0;

  // Count entries per row
  std::vector<int> row_count(static_cast<size_t>(nrows), 0);
  for (int k = 0; k < nnz; ++k) {
    ++row_count[static_cast<size_t>(matind[k])];
  }

  // Build row begin pointers
  std::vector<int> cbeg(static_cast<size_t>(nrows) + 1, 0);
  for (int i = 0; i < nrows; ++i) {
    cbeg[static_cast<size_t>(i) + 1] =
        cbeg[static_cast<size_t>(i)] + row_count[static_cast<size_t>(i)];
  }

  // Fill CSR arrays
  std::vector<int> cind(static_cast<size_t>(nnz));
  std::vector<double> cval(static_cast<size_t>(nnz));
  std::vector<int> pos(static_cast<size_t>(nrows), 0);  // current fill position

  if (have_nnz) {
    for (int col = 0; col < ncols; ++col) {
      const int col_start = matbeg[col];
      const int col_end = matbeg[col + 1];
      for (int k = col_start; k < col_end; ++k) {
        const int row = matind[k];
        const auto row_idx = static_cast<size_t>(row);
        const int dest = cbeg[row_idx] + pos[row_idx];
        cind[static_cast<size_t>(dest)] = col;
        cval[static_cast<size_t>(dest)] = matval[k];
        ++pos[row_idx];
      }
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  // Build lower/upper arrays for range constraints
  std::vector<double> lower(static_cast<size_t>(nrows));
  std::vector<double> upper(static_cast<size_t>(nrows));
  for (int i = 0; i < nrows; ++i) {
    const auto idx = static_cast<size_t>(i);
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    lower[idx] = rowlb[i];
    upper[idx] = rowub[i];
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }

  rc = MDOaddrangeconstrs(m_model_,
                          nrows,
                          nnz,
                          cbeg.data(),
                          cind.data(),
                          cval.data(),
                          lower.data(),
                          upper.data(),
                          nullptr);
  check_error(rc, "MDOaddrangeconstrs");
}

// ── dimensions ───────────────────────────────────────────────────────────

int MindOptSolverBackend::get_num_cols() const
{
  int ncols = 0;
  MDOgetintattr(m_model_, MDO_INT_ATTR_NUM_VARS, &ncols);
  return ncols;
}

int MindOptSolverBackend::get_num_rows() const
{
  int nrows = 0;
  MDOgetintattr(m_model_, MDO_INT_ATTR_NUM_CONSS, &nrows);
  return nrows;
}

// ── column ops ───────────────────────────────────────────────────────────

void MindOptSolverBackend::add_col(double lb, double ub, double obj)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  const int rc = MDOaddvar(
      m_model_, 0, nullptr, nullptr, obj, lb, ub, MDO_CONTINUOUS, nullptr);
  check_error(rc, "MDOaddvar");
}

void MindOptSolverBackend::set_col_lower(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_LB, index, value);
}

void MindOptSolverBackend::set_col_upper(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_UB, index, value);
}

void MindOptSolverBackend::set_obj_coeff(int index, double value)
{
  m_prob_cached_ = false;
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_OBJ, index, value);
}

// ── row ops ──────────────────────────────────────────────────────────────

void MindOptSolverBackend::add_row(int num_elements,
                                   const int* columns,
                                   const double* elements,
                                   double rowlb,
                                   double rowub)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  const int rc = MDOaddrangeconstr(m_model_,
                                   num_elements,
                                   const_cast<int*>(columns),  // NOLINT
                                   const_cast<double*>(elements),  // NOLINT
                                   rowlb,
                                   rowub,
                                   nullptr);
  check_error(rc, "MDOaddrangeconstr");
}

void MindOptSolverBackend::add_rows(int num_rows,
                                    const int* rowbeg,
                                    const int* rowind,
                                    const double* rowval,
                                    const double* rowlb,
                                    const double* rowub)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  // MindOpt does not expose a CSR bulk addRows, dispatch per row.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (const int r : iota_range(0, num_rows)) {
    const int start = rowbeg[r];
    const int count = rowbeg[r + 1] - start;
    const int rc =
        MDOaddrangeconstr(m_model_,
                          count,
                          const_cast<int*>(rowind + start),  // NOLINT
                          const_cast<double*>(rowval + start),  // NOLINT
                          rowlb[r],
                          rowub[r],
                          nullptr);
    check_error(rc, "MDOaddrangeconstr");
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

void MindOptSolverBackend::set_row_lower(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_LHS, index, value);
}

void MindOptSolverBackend::set_row_upper(int index, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_RHS, index, value);
}

void MindOptSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_LHS, index, lb);
  MDOsetdblattrelement(m_model_, MDO_DBL_ATTR_RHS, index, ub);
}

void MindOptSolverBackend::delete_rows(int num, const int* indices)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;

  // MDOdelconstrs takes a mutable int* and may modify the array in-place
  // (e.g. sorting).  Copy to protect the caller's data.
  std::vector<int> buf(indices, indices + num);  // NOLINT
  const int rc = MDOdelconstrs(m_model_, num, buf.data());
  check_error(rc, "MDOdelconstrs");
}

// ── coefficients ─────────────────────────────────────────────────────────

double MindOptSolverBackend::get_coeff(int row, int col) const
{
  double value = 0.0;
  MDOgetcoeff(m_model_, row, col, &value);
  return value;
}

void MindOptSolverBackend::set_coeff(int row, int col, double value)
{
  m_prob_cached_ = false;
  m_sol_cached_ = false;
  int cind = row;
  int vind = col;
  double val = value;
  MDOchgcoeffs(m_model_, 1, &cind, &vind, &val);
}

bool MindOptSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

// ── variable types ───────────────────────────────────────────────────────

void MindOptSolverBackend::set_continuous(int index)
{
  m_sol_cached_ = false;
  MDOsetcharattrelement(m_model_, MDO_CHAR_ATTR_VTYPE, index, MDO_CONTINUOUS);
}

void MindOptSolverBackend::set_integer(int index)
{
  m_sol_cached_ = false;
  MDOsetcharattrelement(m_model_, MDO_CHAR_ATTR_VTYPE, index, MDO_INTEGER);
}

bool MindOptSolverBackend::is_continuous(int index) const
{
  char vtype = MDO_CONTINUOUS;
  MDOgetcharattrelement(m_model_, MDO_CHAR_ATTR_VTYPE, index, &vtype);
  return vtype == MDO_CONTINUOUS;
}

bool MindOptSolverBackend::is_integer(int index) const
{
  return !is_continuous(index);
}

// ── cache helpers ────────────────────────────────────────────────────────

void MindOptSolverBackend::cache_problem_data() const
{
  if (m_prob_cached_) {
    return;
  }

  const int ncols = get_num_cols();
  const int nrows = get_num_rows();

  m_collb_.resize(static_cast<size_t>(ncols));
  m_colub_.resize(static_cast<size_t>(ncols));
  m_obj_.resize(static_cast<size_t>(ncols));

  if (ncols > 0) {
    MDOgetdblattrarray(m_model_, MDO_DBL_ATTR_LB, 0, ncols, m_collb_.data());
    MDOgetdblattrarray(m_model_, MDO_DBL_ATTR_UB, 0, ncols, m_colub_.data());
    MDOgetdblattrarray(m_model_, MDO_DBL_ATTR_OBJ, 0, ncols, m_obj_.data());
  }

  m_rowlb_.resize(static_cast<size_t>(nrows));
  m_rowub_.resize(static_cast<size_t>(nrows));

  if (nrows > 0) {
    MDOgetdblattrarray(m_model_, MDO_DBL_ATTR_LHS, 0, nrows, m_rowlb_.data());
    MDOgetdblattrarray(m_model_, MDO_DBL_ATTR_RHS, 0, nrows, m_rowub_.data());
  }

  m_prob_cached_ = true;
}

void MindOptSolverBackend::cache_solution() const
{
  if (m_sol_cached_) {
    return;
  }

  const int ncols = get_num_cols();
  const int nrows = get_num_rows();

  m_col_solution_.resize(static_cast<size_t>(ncols));
  m_reduced_cost_.resize(static_cast<size_t>(ncols));
  m_row_price_.resize(static_cast<size_t>(nrows));

  if (ncols > 0) {
    MDOgetdblattrarray(
        m_model_, MDO_DBL_ATTR_X, 0, ncols, m_col_solution_.data());
    MDOgetdblattrarray(
        m_model_, MDO_DBL_ATTR_RC, 0, ncols, m_reduced_cost_.data());
  }

  if (nrows > 0) {
    MDOgetdblattrarray(
        m_model_, MDO_DBL_ATTR_DUAL_SOLN, 0, nrows, m_row_price_.data());
  }

  m_sol_cached_ = true;
}

// ── solution access ──────────────────────────────────────────────────────

const double* MindOptSolverBackend::col_lower() const
{
  cache_problem_data();
  return m_collb_.data();
}

const double* MindOptSolverBackend::col_upper() const
{
  cache_problem_data();
  return m_colub_.data();
}

const double* MindOptSolverBackend::obj_coefficients() const
{
  cache_problem_data();
  return m_obj_.data();
}

const double* MindOptSolverBackend::row_lower() const
{
  cache_problem_data();
  return m_rowlb_.data();
}

const double* MindOptSolverBackend::row_upper() const
{
  cache_problem_data();
  return m_rowub_.data();
}

const double* MindOptSolverBackend::col_solution() const
{
  cache_solution();
  return m_col_solution_.data();
}

const double* MindOptSolverBackend::reduced_cost() const
{
  cache_solution();
  return m_reduced_cost_.data();
}

const double* MindOptSolverBackend::row_price() const
{
  cache_solution();
  return m_row_price_.data();
}

double MindOptSolverBackend::obj_value() const
{
  double val = 0.0;
  MDOgetdblattr(m_model_, MDO_DBL_ATTR_PRIMAL_OBJ_VAL, &val);
  return val;
}

// ── solution hints ───────────────────────────────────────────────────────

void MindOptSolverBackend::set_col_solution(const double* sol)
{
  if (sol == nullptr) {
    return;
  }
  const auto ncols = static_cast<size_t>(get_num_cols());
  m_col_solution_.assign(
      sol,
      sol + ncols);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_sol_cached_ = true;

  // Provide as warm-start hint via Start attribute
  MDOsetdblattrarray(m_model_,
                     MDO_DBL_ATTR_START,
                     0,
                     static_cast<int>(ncols),
                     const_cast<double*>(sol));  // NOLINT
}

void MindOptSolverBackend::set_row_price(const double* price)
{
  if (price == nullptr) {
    return;
  }
  m_sol_cached_ = false;
  // MindOpt doesn't have a direct dual-start API; ignore silently.
}

// ── solve ────────────────────────────────────────────────────────────────

void MindOptSolverBackend::initial_solve()
{
  m_sol_cached_ = false;
  const int rc = MDOoptimize(m_model_);
  if (rc != MDO_OKAY && rc != MDO_NO_SOLN) {
    check_error(rc, "MDOoptimize");
  }
}

void MindOptSolverBackend::resolve()
{
  m_sol_cached_ = false;
  const int rc = MDOoptimize(m_model_);
  if (rc != MDO_OKAY && rc != MDO_NO_SOLN) {
    check_error(rc, "MDOoptimize");
  }
}

// ── status ───────────────────────────────────────────────────────────────

namespace
{
int mdo_get_status(MDOmodel* model)
{
  int status = MDO_UNKNOWN;
  MDOgetintattr(model, MDO_INT_ATTR_STATUS, &status);
  return status;
}
}  // namespace

bool MindOptSolverBackend::is_proven_optimal() const
{
  return mdo_get_status(m_model_) == MDO_OPTIMAL;
}

bool MindOptSolverBackend::is_abandoned() const
{
  const int stat = mdo_get_status(m_model_);
  return stat == MDO_ITERATION_LIMIT || stat == MDO_TIME_LIMIT
      || stat == MDO_NODE_LIMIT || stat == MDO_INTERRUPTED;
}

bool MindOptSolverBackend::is_proven_primal_infeasible() const
{
  const int stat = mdo_get_status(m_model_);
  return stat == MDO_INFEASIBLE || stat == MDO_INF_OR_UBD;
}

bool MindOptSolverBackend::is_proven_dual_infeasible() const
{
  const int stat = mdo_get_status(m_model_);
  return stat == MDO_UNBOUNDED || stat == MDO_INF_OR_UBD;
}

// ── solver options ───────────────────────────────────────────────────────

void MindOptSolverBackend::apply_options(const SolverOptions& opts)
{
  m_prep_.options = opts;
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;

  apply_options_to_env(m_env_, opts);
}

SolverOptions MindOptSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = true,
      .scaling = SolverScaling::automatic,
      .max_fallbacks = 2,
  };
}

LPAlgo MindOptSolverBackend::get_algorithm() const
{
  return m_algorithm_;
}

int MindOptSolverBackend::get_threads() const
{
  return m_threads_;
}

bool MindOptSolverBackend::get_presolve() const
{
  return m_presolve_;
}

int MindOptSolverBackend::get_log_level() const
{
  return m_log_level_;
}

// ── diagnostics ──────────────────────────────────────────────────────────

std::optional<double> MindOptSolverBackend::get_kappa() const
{
  // MindOpt does not expose a condition number (kappa) query — return
  // nullopt so callers skip this backend in max-kappa aggregation.
  return std::nullopt;
}

// ── logging ──────────────────────────────────────────────────────────────

void MindOptSolverBackend::open_log(FILE* /*file*/, int level)
{
  MDOsetintparam(m_env_, MDO_INT_PAR_OUTPUT_FLAG, level > 0 ? 1 : 0);
}

void MindOptSolverBackend::close_log()
{
  MDOsetintparam(m_env_, MDO_INT_PAR_OUTPUT_FLAG, 0);
}

void MindOptSolverBackend::set_log_filename(const std::string& filename,
                                            int level)
{
  m_prep_.log_filename = filename;
  m_prep_.log_level = level;
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    MDOsetstrparam(m_env_, MDO_STR_PAR_LOG_FILE, log_path.c_str());
    MDOsetintparam(m_env_, MDO_INT_PAR_OUTPUT_FLAG, 1);
  }
}

void MindOptSolverBackend::clear_log_filename()
{
  m_prep_.log_filename.clear();
  m_prep_.log_level = 0;
  MDOsetstrparam(m_env_, MDO_STR_PAR_LOG_FILE, "");
  MDOsetintparam(m_env_, MDO_INT_PAR_OUTPUT_FLAG, 0);
}

// ── names & LP output ────────────────────────────────────────────────────

void MindOptSolverBackend::push_names(const std::vector<std::string>& col_names,
                                      const std::vector<std::string>& row_names)
{
  for (int i = 0; std::cmp_less(i, col_names.size()); ++i) {
    if (!col_names[static_cast<size_t>(i)].empty()) {
      MDOsetstrattrelement(m_model_,
                           MDO_STR_ATTR_VAR_NAME,
                           i,
                           const_cast<char*>(  // NOLINT
                               col_names[static_cast<size_t>(i)].c_str()));
    }
  }

  for (int i = 0; std::cmp_less(i, row_names.size()); ++i) {
    if (!row_names[static_cast<size_t>(i)].empty()) {
      MDOsetstrattrelement(m_model_,
                           MDO_STR_ATTR_CONSTR_NAME,
                           i,
                           const_cast<char*>(  // NOLINT
                               row_names[static_cast<size_t>(i)].c_str()));
    }
  }
}

void MindOptSolverBackend::write_lp(const char* filename)
{
  const auto file = std::format("{}.lp", filename);
  MDOwrite(m_model_, file.c_str());
}

// ── deep copy ────────────────────────────────────────────────────────────

std::unique_ptr<SolverBackend> MindOptSolverBackend::clone() const
{
  auto cloned = std::make_unique<MindOptSolverBackend>();

  if (cloned->m_model_ != nullptr) {
    MDOfreemodel(cloned->m_model_);
  }
  cloned->m_model_ = MDOcopymodel(m_model_);
  if (cloned->m_model_ == nullptr) {
    throw std::runtime_error("MindOpt: MDOcopymodel failed");
  }

  // Replay every cached piece of backend state so the clone owns an env
  // indistinguishable from this one after a load_problem() cycle.  MindOpt
  // env parameters do NOT survive MDOcopymodel (clone owns a fresh env),
  // so this is essential.
  cloned->m_prep_ = m_prep_;
  cloned->m_algorithm_ = m_algorithm_;
  cloned->m_threads_ = m_threads_;
  cloned->m_presolve_ = m_presolve_;
  cloned->m_log_level_ = m_log_level_;

  if (cloned->m_prep_.options.has_value()) {
    apply_options_to_env(cloned->m_env_, *cloned->m_prep_.options);
  }
  apply_log_filename_to_env(
      cloned->m_env_, cloned->m_prep_.log_filename, cloned->m_prep_.log_level);
  if (!cloned->m_prep_.prob_name.empty()) {
    MDOsetstrattr(cloned->m_model_,
                  MDO_STR_ATTR_MODEL_NAME,
                  cloned->m_prep_.prob_name.c_str());
  }

  return cloned;
}

}  // namespace gtopt
