/**
 * @file      gurobi_solver_backend.cpp
 * @brief     Gurobi C API solver backend implementation
 * @date      Wed Apr 16 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <vector>

#include "gurobi_solver_backend.hpp"

#include <fcntl.h>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>
#include <gurobi_c.h>
#include <unistd.h>

namespace gtopt
{

namespace
{

/// Apply every SolverOptions field onto a *fresh* Gurobi env.
///
/// Pure helper: mutates `env` only, touches no backend members.  Shared
/// between the live `apply_options()` path and the clone path, so any
/// option the caller ever set is replayed onto the new env on clone().
///
/// NOTE: SolverOptions::memory_emphasis has no direct Gurobi parameter; the
/// closest analogue is NodefileStart / Threads.  We leave it as a no-op
/// rather than guess — the MindOpt plugin does the same.
void apply_options_to_env(GRBenv* env, const SolverOptions& opts)
{
  if (env == nullptr) {
    return;
  }

  if (opts.threads > 0) {
    GRBsetintparam(env, GRB_INT_PAR_THREADS, opts.threads);
  }

  GRBsetintparam(env, GRB_INT_PAR_PRESOLVE, opts.presolve ? -1 : 0);

  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    GRBsetdblparam(env, GRB_DBL_PAR_OPTIMALITYTOL, *oeps);
  }
  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    GRBsetdblparam(env, GRB_DBL_PAR_FEASIBILITYTOL, *feps);
  }
  if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
    GRBsetdblparam(env, GRB_DBL_PAR_BARCONVTOL, *beps);
  }
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    GRBsetdblparam(env, GRB_DBL_PAR_TIMELIMIT, *tl);
  }

  // Method: -1=auto, 0=primal simplex, 1=dual simplex, 2=barrier, 3=concurrent
  switch (opts.algorithm) {
    case LPAlgo::default_algo:
      GRBsetintparam(env, GRB_INT_PAR_METHOD, GRB_METHOD_AUTO);
      break;
    case LPAlgo::primal:
      GRBsetintparam(env, GRB_INT_PAR_METHOD, GRB_METHOD_PRIMAL);
      break;
    case LPAlgo::dual:
      GRBsetintparam(env, GRB_INT_PAR_METHOD, GRB_METHOD_DUAL);
      break;
    case LPAlgo::barrier:
      GRBsetintparam(env, GRB_INT_PAR_METHOD, GRB_METHOD_BARRIER);
      GRBsetintparam(env, GRB_INT_PAR_CROSSOVER, opts.crossover ? -1 : 0);
      break;
    case LPAlgo::last_algo:
      break;
  }

  GRBsetintparam(env, GRB_INT_PAR_OUTPUTFLAG, opts.log_level > 0 ? 1 : 0);
}

/// Apply cached log-filename settings to a fresh env.  level<=0 or empty
/// filename leaves the env in its silent default.
void apply_log_filename_to_env(GRBenv* env,
                               const std::string& filename,
                               int level)
{
  if (env == nullptr || level <= 0 || filename.empty()) {
    return;
  }
  const auto log_path = std::format("{}.log", filename);
  GRBsetstrparam(env, GRB_STR_PAR_LOGFILE, log_path.c_str());
  GRBsetintparam(env, GRB_INT_PAR_OUTPUTFLAG, 1);
}

/// Check whether a Gurobi license file is reachable at any of the
/// standard search locations.  Without a local license, GRBstartenv
/// falls back to Web License Service (WLS) and may hang for tens of
/// seconds waiting for a network timeout — unacceptable during plugin
/// validation.  This pre-check avoids the hang entirely.
[[nodiscard]] bool has_local_license()
{
  // 1. Explicit path via GRB_LICENSE_FILE
  // NOLINTNEXTLINE(concurrency-mt-unsafe) — read-only getenv
  if (const char* lic = std::getenv("GRB_LICENSE_FILE");
      lic != nullptr && std::filesystem::exists(lic))
  {
    return true;
  }

  // 2. <GUROBI_HOME>/gurobi.lic
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* home = std::getenv("GUROBI_HOME"); home != nullptr
      && std::filesystem::exists(std::format("{}/gurobi.lic", home)))
  {
    return true;
  }

  // 3. ~/gurobi.lic
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* uhome = std::getenv("HOME"); uhome != nullptr
      && std::filesystem::exists(std::format("{}/gurobi.lic", uhome)))
  {
    return true;
  }

  // 4. /opt/gurobi/gurobi.lic
  return std::filesystem::exists("/opt/gurobi/gurobi.lic");
}

/// Start a silent Gurobi env.  The banner ("Set parameter Username…")
/// is emitted at license-check time during GRBstartenv.  We suppress it
/// by setting OutputFlag=0 / LogToConsole=0 on the empty env before
/// starting, and additionally redirect stdout around the start call in
/// case Gurobi writes before the params take effect.
[[nodiscard]] GRBenv* make_silent_env()
{
  if (!has_local_license()) {
    throw std::runtime_error(
        "Gurobi: no license file found. "
        "Set GRB_LICENSE_FILE or place gurobi.lic under $GUROBI_HOME or "
        "$HOME.");
  }

  GRBenv* env = nullptr;

  const int saved_stdout = ::dup(STDOUT_FILENO);  // NOLINT(android-cloexec-dup)
  const int devnull = ::open("/dev/null", O_WRONLY);  // NOLINT
  if (devnull >= 0) {
    ::dup2(devnull, STDOUT_FILENO);
    ::close(devnull);
  }

  int rc = GRBemptyenv(&env);
  if (rc == 0 && env != nullptr) {
    GRBsetintparam(env, GRB_INT_PAR_OUTPUTFLAG, 0);
    GRBsetintparam(env, GRB_INT_PAR_LOGTOCONSOLE, 0);
    rc = GRBstartenv(env);
  }

  if (saved_stdout >= 0) {
    ::dup2(saved_stdout, STDOUT_FILENO);
    ::close(saved_stdout);
  }

  if (rc != 0) {
    const char* msg = (env != nullptr) ? GRBgeterrormsg(env) : "";
    if (env != nullptr) {
      GRBfreeenv(env);
    }
    throw std::runtime_error(
        std::format("Gurobi: env start failed (rc={}: {}). "
                    "Set GUROBI_HOME to your Gurobi installation directory "
                    "and ensure a valid license file (gurobi.lic) is present.",
                    rc,
                    msg != nullptr ? msg : "unknown error"));
  }

  return env;
}

}  // namespace

// ── helpers ──────────────────────────────────────────────────────────────

void GurobiSolverBackend::check_error(int rc, const char* func) const
{
  if (rc != 0) {
    const char* msg = (m_env_ != nullptr) ? GRBgeterrormsg(m_env_) : "";
    throw std::runtime_error(std::format(
        "Gurobi: {} failed (rc={}: {})", func, rc, msg != nullptr ? msg : ""));
  }
}

void GurobiSolverBackend::ensure_updated_() const
{
  if (m_dirty_ && m_model_ != nullptr) {
    GRBupdatemodel(m_model_);
    m_dirty_ = false;
  }
}

void GurobiSolverBackend::reset_model_()
{
  if (m_model_ != nullptr) {
    GRBfreemodel(m_model_);
    m_model_ = nullptr;
  }
  const char* pname =
      m_prep_.prob_name.empty() ? "gtopt" : m_prep_.prob_name.c_str();
  const int rc = GRBnewmodel(
      m_env_, &m_model_, pname, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
  check_error(rc, "GRBnewmodel");
  GRBsetintattr(m_model_, GRB_INT_ATTR_MODELSENSE, GRB_MINIMIZE);
  m_rowlb_local_.clear();
  m_rowub_local_.clear();
  invalidate_problem_data();
  m_dirty_ = true;
}

// ── ctor / dtor ──────────────────────────────────────────────────────────

GurobiSolverBackend::GurobiSolverBackend()
    : m_env_(make_silent_env())
    , m_dirty_(true)
{
  const int rc = GRBnewmodel(m_env_,
                             &m_model_,
                             "gtopt",
                             0,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr);
  check_error(rc, "GRBnewmodel");

  GRBsetintattr(m_model_, GRB_INT_ATTR_MODELSENSE, GRB_MINIMIZE);
}

GurobiSolverBackend::~GurobiSolverBackend()
{
  if (m_model_ != nullptr) {
    GRBfreemodel(m_model_);
  }
  if (m_env_ != nullptr) {
    GRBfreeenv(m_env_);
  }
}

// ── identity ─────────────────────────────────────────────────────────────

std::string_view GurobiSolverBackend::solver_name() const noexcept
{
  return "gurobi";
}

std::string GurobiSolverBackend::solver_version() const
{
  int major = 0;
  int minor = 0;
  int technical = 0;
  GRBversion(&major, &minor, &technical);
  return std::format("{}.{}.{}", major, minor, technical);
}

double GurobiSolverBackend::plugin_infinity() noexcept
{
  return GRB_INFINITY;
}

double GurobiSolverBackend::infinity() const noexcept
{
  return plugin_infinity();
}

bool GurobiSolverBackend::supports_mip() const noexcept
{
  return true;
}

// ── problem name ─────────────────────────────────────────────────────────

void GurobiSolverBackend::set_prob_name(const std::string& name)
{
  m_prep_.prob_name = name;
  GRBsetstrattr(m_model_, GRB_STR_ATTR_MODELNAME, name.c_str());
  m_dirty_ = true;
}

std::string GurobiSolverBackend::get_prob_name() const
{
  ensure_updated_();
  char* name = nullptr;
  if (GRBgetstrattr(m_model_, GRB_STR_ATTR_MODELNAME, &name) == 0
      && name != nullptr)
  {
    return {name};
  }
  return {};
}

// ── bulk load ────────────────────────────────────────────────────────────

void GurobiSolverBackend::load_problem(int ncols,
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

  if (m_model_ != nullptr) {
    GRBfreemodel(m_model_);
    m_model_ = nullptr;
  }

  // Step 1: create model with columns only (no constraints)
  const char* pname =
      m_prep_.prob_name.empty() ? "gtopt" : m_prep_.prob_name.c_str();
  int rc = GRBnewmodel(m_env_,
                       &m_model_,
                       pname,
                       ncols,
                       const_cast<double*>(obj),  // NOLINT
                       const_cast<double*>(collb),  // NOLINT
                       const_cast<double*>(colub),  // NOLINT
                       nullptr,  // vtype: all continuous
                       nullptr);  // varnames
  check_error(rc, "GRBnewmodel");

  GRBsetintattr(m_model_, GRB_INT_ATTR_MODELSENSE, GRB_MINIMIZE);

  // Step 2: convert CSC to CSR and add range constraints in batch
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

  rc = GRBaddrangeconstrs(m_model_,
                          nrows,
                          nnz,
                          cbeg.data(),
                          cind.data(),
                          cval.data(),
                          lower.data(),
                          upper.data(),
                          nullptr);
  check_error(rc, "GRBaddrangeconstrs");

  m_rowlb_local_ = std::move(lower);
  m_rowub_local_ = std::move(upper);
  m_dirty_ = true;
}

// ── dimensions ───────────────────────────────────────────────────────────

int GurobiSolverBackend::get_num_cols() const
{
  ensure_updated_();
  int ncols = 0;
  GRBgetintattr(m_model_, GRB_INT_ATTR_NUMVARS, &ncols);
  return ncols;
}

int GurobiSolverBackend::get_num_rows() const
{
  ensure_updated_();
  int nrows = 0;
  GRBgetintattr(m_model_, GRB_INT_ATTR_NUMCONSTRS, &nrows);
  return nrows;
}

// ── column ops ───────────────────────────────────────────────────────────

void GurobiSolverBackend::add_col(double lb, double ub, double obj)
{
  invalidate_problem_data();
  const int rc = GRBaddvar(
      m_model_, 0, nullptr, nullptr, obj, lb, ub, GRB_CONTINUOUS, nullptr);
  check_error(rc, "GRBaddvar");
  m_dirty_ = true;
}

void GurobiSolverBackend::add_cols(int num_cols,
                                   const int* colbeg,
                                   const int* colind,
                                   const double* colval,
                                   const double* collb,
                                   const double* colub,
                                   const double* colobj)
{
  invalidate_problem_data();

  if (num_cols == 0) {
    return;
  }

  // Gurobi's CSC bulk variable API: GRBaddvars consumes the same
  // (vbeg, vind, vval, obj, lb, ub, vtype, varnames) shape we hand it.
  // All new vars are continuous (vtype=nullptr defaults to 'C').  Const-
  // cast is safe — GRB's API is non-modifying despite the non-const
  // pointer signature (mirrors the existing `GRBaddrangeconstrs` call).
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int nnz = colbeg[num_cols];
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int rc = GRBaddvars(m_model_,
                            num_cols,
                            nnz,
                            const_cast<int*>(colbeg),  // NOLINT
                            const_cast<int*>(colind),  // NOLINT
                            const_cast<double*>(colval),  // NOLINT
                            const_cast<double*>(colobj),  // NOLINT
                            const_cast<double*>(collb),  // NOLINT
                            const_cast<double*>(colub),  // NOLINT
                            nullptr,  // vtype: all continuous
                            nullptr);  // varnames
  check_error(rc, "GRBaddvars");
  m_dirty_ = true;
}

void GurobiSolverBackend::set_col_lower(int index, double value)
{
  invalidate_problem_data();
  GRBsetdblattrelement(m_model_, GRB_DBL_ATTR_LB, index, value);
  m_dirty_ = true;
}

void GurobiSolverBackend::set_col_upper(int index, double value)
{
  invalidate_problem_data();
  GRBsetdblattrelement(m_model_, GRB_DBL_ATTR_UB, index, value);
  m_dirty_ = true;
}

void GurobiSolverBackend::set_obj_coeff(int index, double value)
{
  invalidate_problem_data();
  GRBsetdblattrelement(m_model_, GRB_DBL_ATTR_OBJ, index, value);
  m_dirty_ = true;
}

void GurobiSolverBackend::set_obj_coeffs(const double* values, int num_cols)
{
  // `GRBsetdblattrarray(model, attr, first, len, values)` is range-based.
  // The C API is not const-correct on the values pointer, so cast.
  invalidate_problem_data();
  if (num_cols <= 0) {
    return;
  }
  GRBsetdblattrarray(m_model_,
                     GRB_DBL_ATTR_OBJ,
                     0,
                     num_cols,
                     // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                     const_cast<double*>(values));
  m_dirty_ = true;
}

// ── row ops ──────────────────────────────────────────────────────────────

void GurobiSolverBackend::add_row(int num_elements,
                                  const int* columns,
                                  const double* elements,
                                  double rowlb,
                                  double rowub)
{
  invalidate_problem_data();

  const int rc = GRBaddrangeconstr(m_model_,
                                   num_elements,
                                   const_cast<int*>(columns),  // NOLINT
                                   const_cast<double*>(elements),  // NOLINT
                                   rowlb,
                                   rowub,
                                   nullptr);
  check_error(rc, "GRBaddrangeconstr");
  m_rowlb_local_.push_back(rowlb);
  m_rowub_local_.push_back(rowub);
  m_dirty_ = true;
}

void GurobiSolverBackend::add_rows(int num_rows,
                                   const int* rowbeg,
                                   const int* rowind,
                                   const double* rowval,
                                   const double* rowlb,
                                   const double* rowub)
{
  invalidate_problem_data();

  if (num_rows == 0) {
    return;
  }

  // Gurobi's CSR bulk constraint API: same shape as the LinearInterface
  // hands us.  Already used in `load_problem` for the structural build;
  // reusing it here keeps cut replay (apply_post_load_replay) a single
  // solver call instead of N.  Const-cast is safe — GRB's API is
  // non-modifying despite the non-const pointer signature (mirrors the
  // existing `GRBaddrangeconstrs` call site in `load_problem`).
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int nnz = rowbeg[num_rows];
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int rc = GRBaddrangeconstrs(m_model_,
                                    num_rows,
                                    nnz,
                                    const_cast<int*>(rowbeg),  // NOLINT
                                    const_cast<int*>(rowind),  // NOLINT
                                    const_cast<double*>(rowval),  // NOLINT
                                    const_cast<double*>(rowlb),  // NOLINT
                                    const_cast<double*>(rowub),  // NOLINT
                                    nullptr);
  check_error(rc, "GRBaddrangeconstrs");

  // Append the new bounds to the local mirror so set_row_lower /
  // set_row_upper / set_row_bounds keep returning correct values.
  // The previous per-row loop appended one bound per iteration; the
  // bulk path just appends in one chunk.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_rowlb_local_.insert(m_rowlb_local_.end(), rowlb, rowlb + num_rows);
  m_rowub_local_.insert(m_rowub_local_.end(), rowub, rowub + num_rows);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  m_dirty_ = true;
}

void GurobiSolverBackend::set_row_lower(int index, double value)
{
  invalidate_problem_data();
  const auto idx = static_cast<size_t>(index);
  if (idx < m_rowlb_local_.size()) {
    m_rowlb_local_[idx] = value;
  }
  // Gurobi represents ranges via slack vars; simplest modification is to
  // rewrite both bounds via the range API.  Here we only update the lower
  // bound; rewriting the row would require knowing the slack index.  Most
  // gtopt flows set bounds via set_row_bounds(), so take that path when
  // both bounds must be synchronized.
  GRBsetdblattrelement(m_model_, GRB_DBL_ATTR_RHS, index, value);
  m_dirty_ = true;
}

void GurobiSolverBackend::set_row_upper(int index, double value)
{
  invalidate_problem_data();
  const auto idx = static_cast<size_t>(index);
  if (idx < m_rowub_local_.size()) {
    m_rowub_local_[idx] = value;
  }
  GRBsetdblattrelement(m_model_, GRB_DBL_ATTR_RHS, index, value);
  m_dirty_ = true;
}

void GurobiSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  invalidate_problem_data();
  const auto idx = static_cast<size_t>(index);
  if (idx < m_rowlb_local_.size()) {
    m_rowlb_local_[idx] = lb;
  }
  if (idx < m_rowub_local_.size()) {
    m_rowub_local_[idx] = ub;
  }
  // Equality / single-bound rows map directly; true ranges (lb < ub)
  // require the slack-variable mechanism.  For now write RHS = ub and
  // rely on the caller's sense stored at creation time.
  GRBsetdblattrelement(m_model_, GRB_DBL_ATTR_RHS, index, ub);
  m_dirty_ = true;
}

void GurobiSolverBackend::delete_rows(int num, const int* indices)
{
  invalidate_problem_data();

  // Gurobi mutates the index array in-place (sorts it); copy to protect
  // caller data.
  std::vector<int> buf(indices, indices + num);  // NOLINT
  const int rc = GRBdelconstrs(m_model_, num, buf.data());
  check_error(rc, "GRBdelconstrs");

  // Sort the (now-copied) indices descending so erases don't shift later
  // positions.
  std::ranges::sort(buf, std::greater<> {});
  for (const int i : buf) {
    const auto idx = static_cast<size_t>(i);
    if (idx < m_rowlb_local_.size()) {
      m_rowlb_local_.erase(m_rowlb_local_.begin()
                           + static_cast<std::ptrdiff_t>(idx));
    }
    if (idx < m_rowub_local_.size()) {
      m_rowub_local_.erase(m_rowub_local_.begin()
                           + static_cast<std::ptrdiff_t>(idx));
    }
  }
  m_dirty_ = true;
}

// ── coefficients ─────────────────────────────────────────────────────────

double GurobiSolverBackend::get_coeff(int row, int col) const
{
  ensure_updated_();
  double value = 0.0;
  GRBgetcoeff(m_model_, row, col, &value);
  return value;
}

void GurobiSolverBackend::set_coeff(int row, int col, double value)
{
  invalidate_problem_data();
  int cind = row;
  int vind = col;
  double val = value;
  GRBchgcoeffs(m_model_, 1, &cind, &vind, &val);
  m_dirty_ = true;
}

bool GurobiSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

// ── variable types ───────────────────────────────────────────────────────

void GurobiSolverBackend::set_continuous(int index)
{
  GRBsetcharattrelement(m_model_, GRB_CHAR_ATTR_VTYPE, index, GRB_CONTINUOUS);
  m_dirty_ = true;
}

void GurobiSolverBackend::set_integer(int index)
{
  GRBsetcharattrelement(m_model_, GRB_CHAR_ATTR_VTYPE, index, GRB_INTEGER);
  m_dirty_ = true;
}

bool GurobiSolverBackend::is_continuous(int index) const
{
  ensure_updated_();
  char vtype = GRB_CONTINUOUS;
  GRBgetcharattrelement(m_model_, GRB_CHAR_ATTR_VTYPE, index, &vtype);
  return vtype == GRB_CONTINUOUS;
}

bool GurobiSolverBackend::is_integer(int index) const
{
  return !is_continuous(index);
}

// ── cache helpers ────────────────────────────────────────────────────────

void GurobiSolverBackend::invalidate_problem_data() const noexcept
{
  m_collb_cached_ = false;
  m_colub_cached_ = false;
  m_obj_cached_ = false;
}

void GurobiSolverBackend::fill_collb_if_needed() const
{
  if (m_collb_cached_) {
    return;
  }
  ensure_updated_();
  const int ncols = get_num_cols();
  m_collb_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    GRBgetdblattrarray(m_model_, GRB_DBL_ATTR_LB, 0, ncols, m_collb_.data());
  }
  m_collb_cached_ = true;
}

void GurobiSolverBackend::fill_colub_if_needed() const
{
  if (m_colub_cached_) {
    return;
  }
  ensure_updated_();
  const int ncols = get_num_cols();
  m_colub_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    GRBgetdblattrarray(m_model_, GRB_DBL_ATTR_UB, 0, ncols, m_colub_.data());
  }
  m_colub_cached_ = true;
}

void GurobiSolverBackend::fill_obj_if_needed() const
{
  if (m_obj_cached_) {
    return;
  }
  ensure_updated_();
  const int ncols = get_num_cols();
  m_obj_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    GRBgetdblattrarray(m_model_, GRB_DBL_ATTR_OBJ, 0, ncols, m_obj_.data());
  }
  m_obj_cached_ = true;
}

// ── solution access ──────────────────────────────────────────────────────

const double* GurobiSolverBackend::col_lower() const
{
  fill_collb_if_needed();
  return m_collb_.data();
}

const double* GurobiSolverBackend::col_upper() const
{
  fill_colub_if_needed();
  return m_colub_.data();
}

const double* GurobiSolverBackend::obj_coefficients() const
{
  fill_obj_if_needed();
  return m_obj_.data();
}

const double* GurobiSolverBackend::row_lower() const
{
  return m_rowlb_local_.data();
}

const double* GurobiSolverBackend::row_upper() const
{
  return m_rowub_local_.data();
}

// Solution accessors: each refills *only its own* buffer via the
// matching GRBgetdblattrarray.  No cross-accessor work, no validity
// flag, no caching semantics — the buffer is plain scratch storage
// for the C-API write target.  The single caching layer is in
// `LinearInterface::populate_solution_cache_post_solve`.
const double* GurobiSolverBackend::col_solution() const
{
  ensure_updated_();
  const int ncols = get_num_cols();
  m_col_solution_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    GRBgetdblattrarray(
        m_model_, GRB_DBL_ATTR_X, 0, ncols, m_col_solution_.data());
  }
  return m_col_solution_.data();
}

const double* GurobiSolverBackend::reduced_cost() const
{
  ensure_updated_();
  const int ncols = get_num_cols();
  m_reduced_cost_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    GRBgetdblattrarray(
        m_model_, GRB_DBL_ATTR_RC, 0, ncols, m_reduced_cost_.data());
  }
  return m_reduced_cost_.data();
}

const double* GurobiSolverBackend::row_price() const
{
  ensure_updated_();
  const int nrows = get_num_rows();
  m_row_price_.resize(static_cast<size_t>(nrows));
  if (nrows > 0) {
    GRBgetdblattrarray(
        m_model_, GRB_DBL_ATTR_PI, 0, nrows, m_row_price_.data());
  }
  return m_row_price_.data();
}

// Span-out fills: write directly into the caller's buffer via the
// Gurobi C-API, never touching the per-instance scratch members.

void GurobiSolverBackend::fill_col_sol(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  ensure_updated_();
  GRBgetdblattrarray(
      m_model_, GRB_DBL_ATTR_X, 0, static_cast<int>(out.size()), out.data());
}

void GurobiSolverBackend::fill_col_cost(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  ensure_updated_();
  GRBgetdblattrarray(
      m_model_, GRB_DBL_ATTR_RC, 0, static_cast<int>(out.size()), out.data());
}

void GurobiSolverBackend::fill_row_dual(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  ensure_updated_();
  GRBgetdblattrarray(
      m_model_, GRB_DBL_ATTR_PI, 0, static_cast<int>(out.size()), out.data());
}

double GurobiSolverBackend::obj_value() const
{
  ensure_updated_();
  double val = 0.0;
  GRBgetdblattr(m_model_, GRB_DBL_ATTR_OBJVAL, &val);
  return val;
}

// ── solution hints ───────────────────────────────────────────────────────

void GurobiSolverBackend::set_col_solution(const double* sol)
{
  if (sol == nullptr) {
    return;
  }
  ensure_updated_();
  const auto ncols = static_cast<size_t>(get_num_cols());

  // Provide as warm-start hint via Start attribute.  No backend-side
  // primal cache exists anymore; the next col_solution() after a
  // successful resolve will re-fetch via GRB_DBL_ATTR_X.
  GRBsetdblattrarray(m_model_,
                     GRB_DBL_ATTR_START,
                     0,
                     static_cast<int>(ncols),
                     const_cast<double*>(sol));  // NOLINT
  m_dirty_ = true;
}

void GurobiSolverBackend::set_row_price(const double* price)
{
  if (price == nullptr) {
    return;
  }
  // Gurobi does not expose a dual-start API for LPs; the DStart attribute
  // exists but is advisory for warm-start.  Ignore silently for now.
}

// ── solve ────────────────────────────────────────────────────────────────

void GurobiSolverBackend::initial_solve()
{
  ensure_updated_();
  const int rc = GRBoptimize(m_model_);
  if (rc != 0) {
    check_error(rc, "GRBoptimize");
  }
}

void GurobiSolverBackend::resolve()
{
  ensure_updated_();
  const int rc = GRBoptimize(m_model_);
  if (rc != 0) {
    check_error(rc, "GRBoptimize");
  }
}

void GurobiSolverBackend::engage_robust_solve()
{
  if (m_env_ == nullptr) {
    return;
  }

  if (!m_saved_robust_state_.has_value()) {
    RobustState saved {};
    GRBgetdblparam(m_env_, GRB_DBL_PAR_OPTIMALITYTOL, &saved.optimality_tol);
    GRBgetdblparam(m_env_, GRB_DBL_PAR_FEASIBILITYTOL, &saved.feasibility_tol);
    GRBgetdblparam(m_env_, GRB_DBL_PAR_BARCONVTOL, &saved.bar_conv_tol);
    GRBgetintparam(m_env_, GRB_INT_PAR_NUMERICFOCUS, &saved.numeric_focus);
    saved.engage_count = 0;
    m_saved_robust_state_ = saved;
  }
  ++m_saved_robust_state_->engage_count;

  double cur_opt = 0.0;
  double cur_feas = 0.0;
  double cur_bar = 0.0;
  GRBgetdblparam(m_env_, GRB_DBL_PAR_OPTIMALITYTOL, &cur_opt);
  GRBgetdblparam(m_env_, GRB_DBL_PAR_FEASIBILITYTOL, &cur_feas);
  GRBgetdblparam(m_env_, GRB_DBL_PAR_BARCONVTOL, &cur_bar);

  // Gurobi caps OptimalityTol/FeasibilityTol at 1e-2; BarConvTol at 1.0.
  constexpr double k_max_simplex_tol = 1e-2;
  constexpr double k_max_barrier_tol = 1.0;
  GRBsetdblparam(m_env_,
                 GRB_DBL_PAR_OPTIMALITYTOL,
                 std::min(cur_opt * 10.0, k_max_simplex_tol));
  GRBsetdblparam(m_env_,
                 GRB_DBL_PAR_FEASIBILITYTOL,
                 std::min(cur_feas * 10.0, k_max_simplex_tol));
  GRBsetdblparam(m_env_,
                 GRB_DBL_PAR_BARCONVTOL,
                 std::min(cur_bar * 10.0, k_max_barrier_tol));

  // NumericFocus: 0=auto, 1=careful, 2=more careful, 3=most careful.
  GRBsetintparam(m_env_, GRB_INT_PAR_NUMERICFOCUS, 2);
}

void GurobiSolverBackend::disengage_robust_solve() noexcept
{
  if (!m_saved_robust_state_.has_value()) {
    return;
  }
  if (m_env_ == nullptr) {
    m_saved_robust_state_.reset();
    return;
  }

  const auto& s = *m_saved_robust_state_;
  GRBsetdblparam(m_env_, GRB_DBL_PAR_OPTIMALITYTOL, s.optimality_tol);
  GRBsetdblparam(m_env_, GRB_DBL_PAR_FEASIBILITYTOL, s.feasibility_tol);
  GRBsetdblparam(m_env_, GRB_DBL_PAR_BARCONVTOL, s.bar_conv_tol);
  GRBsetintparam(m_env_, GRB_INT_PAR_NUMERICFOCUS, s.numeric_focus);
  m_saved_robust_state_.reset();
}

// ── status ───────────────────────────────────────────────────────────────

namespace
{
int grb_get_status(GRBmodel* model)
{
  int status = 0;
  GRBgetintattr(model, GRB_INT_ATTR_STATUS, &status);
  return status;
}
}  // namespace

bool GurobiSolverBackend::is_proven_optimal() const
{
  return grb_get_status(m_model_) == GRB_OPTIMAL;
}

bool GurobiSolverBackend::is_abandoned() const
{
  const int stat = grb_get_status(m_model_);
  return stat == GRB_ITERATION_LIMIT || stat == GRB_TIME_LIMIT
      || stat == GRB_NODE_LIMIT || stat == GRB_INTERRUPTED
      || stat == GRB_WORK_LIMIT || stat == GRB_MEM_LIMIT;
}

bool GurobiSolverBackend::is_proven_primal_infeasible() const
{
  const int stat = grb_get_status(m_model_);
  return stat == GRB_INFEASIBLE || stat == GRB_INF_OR_UNBD;
}

bool GurobiSolverBackend::is_proven_dual_infeasible() const
{
  const int stat = grb_get_status(m_model_);
  return stat == GRB_UNBOUNDED || stat == GRB_INF_OR_UNBD;
}

// ── solver options ───────────────────────────────────────────────────────

void GurobiSolverBackend::apply_options(const SolverOptions& opts)
{
  m_prep_.options = opts;
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;

  apply_options_to_env(m_env_, opts);
}

SolverOptions GurobiSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = true,
      .scaling = SolverScaling::automatic,
      .max_fallbacks = 2,
  };
}

LPAlgo GurobiSolverBackend::get_algorithm() const
{
  return m_algorithm_;
}

int GurobiSolverBackend::get_threads() const
{
  return m_threads_;
}

bool GurobiSolverBackend::get_presolve() const
{
  return m_presolve_;
}

int GurobiSolverBackend::get_log_level() const
{
  return m_log_level_;
}

// ── diagnostics ──────────────────────────────────────────────────────────

std::optional<double> GurobiSolverBackend::get_kappa() const
{
  double kappa = 0.0;
  if (GRBgetdblattr(m_model_, GRB_DBL_ATTR_KAPPA, &kappa) != 0) {
    return std::nullopt;
  }
  return kappa;
}

// ── logging ──────────────────────────────────────────────────────────────

void GurobiSolverBackend::open_log(FILE* /*file*/, int level)
{
  GRBsetintparam(m_env_, GRB_INT_PAR_OUTPUTFLAG, level > 0 ? 1 : 0);
}

void GurobiSolverBackend::close_log()
{
  GRBsetintparam(m_env_, GRB_INT_PAR_OUTPUTFLAG, 0);
}

void GurobiSolverBackend::set_log_filename(const std::string& filename,
                                           int level)
{
  m_prep_.log_filename = filename;
  m_prep_.log_level = level;
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    GRBsetstrparam(m_env_, GRB_STR_PAR_LOGFILE, log_path.c_str());
    GRBsetintparam(m_env_, GRB_INT_PAR_OUTPUTFLAG, 1);
  }
}

void GurobiSolverBackend::clear_log_filename()
{
  m_prep_.log_filename.clear();
  m_prep_.log_level = 0;
  GRBsetstrparam(m_env_, GRB_STR_PAR_LOGFILE, "");
  GRBsetintparam(m_env_, GRB_INT_PAR_OUTPUTFLAG, 0);
}

// ── names & LP output ────────────────────────────────────────────────────

void GurobiSolverBackend::push_names(const std::vector<std::string>& col_names,
                                     const std::vector<std::string>& row_names)
{
  ensure_updated_();
  for (int i = 0; std::cmp_less(i, col_names.size()); ++i) {
    if (!col_names[static_cast<size_t>(i)].empty()) {
      GRBsetstrattrelement(m_model_,
                           GRB_STR_ATTR_VARNAME,
                           i,
                           col_names[static_cast<size_t>(i)].c_str());
    }
  }

  for (int i = 0; std::cmp_less(i, row_names.size()); ++i) {
    if (!row_names[static_cast<size_t>(i)].empty()) {
      GRBsetstrattrelement(m_model_,
                           GRB_STR_ATTR_CONSTRNAME,
                           i,
                           row_names[static_cast<size_t>(i)].c_str());
    }
  }
  m_dirty_ = true;
}

void GurobiSolverBackend::write_lp(const char* filename)
{
  ensure_updated_();
  const auto file = std::format("{}.lp", filename);
  GRBwrite(m_model_, file.c_str());
}

// ── deep copy ────────────────────────────────────────────────────────────

std::unique_ptr<SolverBackend> GurobiSolverBackend::clone() const
{
  ensure_updated_();

  auto cloned = std::make_unique<GurobiSolverBackend>();

  // Replace the clone's empty model with a copy of ours, targeted to its
  // own env so the clone owns a self-contained (env,model) pair.
  if (cloned->m_model_ != nullptr) {
    GRBfreemodel(cloned->m_model_);
    cloned->m_model_ = nullptr;
  }
  const int rc = GRBcopymodeltoenv(m_model_, cloned->m_env_, &cloned->m_model_);
  if (rc != 0 || cloned->m_model_ == nullptr) {
    const char* msg = GRBgeterrormsg(m_env_);
    throw std::runtime_error(
        std::format("Gurobi: GRBcopymodeltoenv failed (rc={}: {})",
                    rc,
                    msg != nullptr ? msg : ""));
  }

  // Replay every cached piece of backend state.  Gurobi env parameters do
  // NOT survive into the clone (clone owns a fresh env), so this is
  // essential.
  cloned->m_prep_ = m_prep_;
  cloned->m_algorithm_ = m_algorithm_;
  cloned->m_threads_ = m_threads_;
  cloned->m_presolve_ = m_presolve_;
  cloned->m_log_level_ = m_log_level_;
  cloned->m_rowlb_local_ = m_rowlb_local_;
  cloned->m_rowub_local_ = m_rowub_local_;

  if (cloned->m_prep_.options.has_value()) {
    apply_options_to_env(cloned->m_env_, *cloned->m_prep_.options);
  }
  apply_log_filename_to_env(
      cloned->m_env_, cloned->m_prep_.log_filename, cloned->m_prep_.log_level);
  if (!cloned->m_prep_.prob_name.empty()) {
    GRBsetstrattr(cloned->m_model_,
                  GRB_STR_ATTR_MODELNAME,
                  cloned->m_prep_.prob_name.c_str());
  }

  cloned->m_dirty_ = true;
  return cloned;
}

}  // namespace gtopt
