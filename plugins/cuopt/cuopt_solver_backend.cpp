/**
 * @file      cuopt_solver_backend.cpp
 * @brief     NVIDIA cuOpt (GPU) solver backend implementation (SCAFFOLD)
 * @date      Tue Jun 10 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * See cuopt_solver_backend.hpp for the buffer-and-replay design rationale.
 * The mutator buffering, CSR lowering, GPU solve, and primal/dual/RC
 * snapshot are implemented here.  Items still requiring validation /
 * hardening are flagged `TODO(cuopt)` and summarized at the bottom.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cuopt_solver_backend.hpp"

#include <cuopt/linear_programming/constants.h>
#include <cuopt/linear_programming/cuopt_c.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// gtopt's conventional "no bound" sentinel.  cuOpt's own CUOPT_INFINITY is
/// `inf`, but gtopt data and `auto_scale_*` use 1e30, so we report 1e30 from
/// `infinity()` / `plugin_infinity()` and clamp magnitudes >= 1e30 to
/// CUOPT_INFINITY when lowering bounds into the problem object.
constexpr double k_gtopt_inf = 1e30;

/// Map gtopt magnitudes at/above the sentinel to cuOpt's infinity so the
/// GPU solver treats them as "unbounded" rather than a finite 1e30 bound.
[[nodiscard]] double to_cuopt_bound(double v) noexcept
{
  if (v >= k_gtopt_inf) {
    return CUOPT_INFINITY;
  }
  if (v <= -k_gtopt_inf) {
    return -CUOPT_INFINITY;
  }
  return v;
}

/// Log and throw on a non-success cuOpt status code, naming the failing call.
/// Follows the gtopt convention (format the message once, log it, then throw
/// the same text) so cuOpt API failures land in the log before propagating.
void check(cuopt_int_t status, const char* what)
{
  if (status != CUOPT_SUCCESS) {
    auto msg = std::format("cuopt: {} failed with status {}", what, status);
    spdlog::error(msg);
    throw std::runtime_error(std::move(msg));
  }
}

}  // namespace

CuOptSolverBackend::CuOptSolverBackend() = default;
CuOptSolverBackend::~CuOptSolverBackend() = default;

// ---- identity -------------------------------------------------------------

std::string_view CuOptSolverBackend::solver_name() const noexcept
{
  return "cuopt";
}

std::string CuOptSolverBackend::solver_version() const
{
  cuopt_int_t maj = 0;
  cuopt_int_t min = 0;
  cuopt_int_t pat = 0;
  cuOptGetVersion(&maj, &min, &pat);
  return std::format("{}.{}.{}", maj, min, pat);
}

double CuOptSolverBackend::infinity() const noexcept
{
  return k_gtopt_inf;
}

double CuOptSolverBackend::plugin_infinity() noexcept
{
  return k_gtopt_inf;
}

bool CuOptSolverBackend::supports_mip() const noexcept
{
  return true;  // cuOpt has a GPU branch-and-bound MIP engine.
}

// ---- problem name ---------------------------------------------------------

void CuOptSolverBackend::set_prob_name(const std::string& name)
{
  m_prob_name_ = name;
}

std::string CuOptSolverBackend::get_prob_name() const
{
  return m_prob_name_;
}

// ---- bulk load ------------------------------------------------------------
//
// gtopt hands us the matrix in CSC (Compressed Sparse Column):
//   matbeg[j] .. matbeg[j+1]   → entries of column j
//   matind[k]                  → row index of entry k
//   matval[k]                  → value of entry k
// We transpose into our row-major `CuOptModel`.

void CuOptSolverBackend::load_problem(int ncols,
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
  m_model_ = CuOptModel {};
  m_model_.num_cols = ncols;
  m_model_.num_rows = nrows;

  m_model_.col_lb.assign(collb, collb + ncols);
  m_model_.col_ub.assign(colub, colub + ncols);
  m_model_.col_obj.assign(obj, obj + ncols);
  m_model_.col_type.assign(static_cast<size_t>(ncols), CUOPT_CONTINUOUS);

  m_model_.row_lb.assign(rowlb, rowlb + nrows);
  m_model_.row_ub.assign(rowub, rowub + nrows);
  m_model_.row_entries.assign(static_cast<size_t>(nrows), {});

  for (int j = 0; j < ncols; ++j) {
    for (int k = matbeg[j]; k < matbeg[j + 1]; ++k) {
      const int i = matind[k];
      m_model_.row_entries[static_cast<size_t>(i)][j] = matval[k];
    }
  }
  m_sol_ = CuOptSolutionCache {};
}

// ---- dimensions -----------------------------------------------------------

int CuOptSolverBackend::get_num_cols() const
{
  return m_model_.num_cols;
}

int CuOptSolverBackend::get_num_rows() const
{
  return m_model_.num_rows;
}

// ---- column ops -----------------------------------------------------------

void CuOptSolverBackend::add_col(double lb, double ub, double obj)
{
  m_model_.col_lb.push_back(lb);
  m_model_.col_ub.push_back(ub);
  m_model_.col_obj.push_back(obj);
  m_model_.col_type.push_back(CUOPT_CONTINUOUS);
  ++m_model_.num_cols;
}

void CuOptSolverBackend::add_cols(int num_cols,
                                  const int* colbeg,
                                  const int* colind,
                                  const double* colval,
                                  const double* collb,
                                  const double* colub,
                                  const double* colobj)
{
  const int base = m_model_.num_cols;
  for (int j = 0; j < num_cols; ++j) {
    const int new_col = base + j;
    m_model_.col_lb.push_back(collb[j]);
    m_model_.col_ub.push_back(colub[j]);
    m_model_.col_obj.push_back(colobj[j]);
    m_model_.col_type.push_back(CUOPT_CONTINUOUS);
    for (int k = colbeg[j]; k < colbeg[j + 1]; ++k) {
      const int i = colind[k];
      m_model_.row_entries[static_cast<size_t>(i)][new_col] = colval[k];
    }
  }
  m_model_.num_cols += num_cols;
}

void CuOptSolverBackend::set_col_lower(int index, double value)
{
  m_model_.col_lb[static_cast<size_t>(index)] = value;
}

void CuOptSolverBackend::set_col_upper(int index, double value)
{
  m_model_.col_ub[static_cast<size_t>(index)] = value;
}

void CuOptSolverBackend::set_obj_coeff(int index, double value)
{
  m_model_.col_obj[static_cast<size_t>(index)] = value;
}

// ---- row ops --------------------------------------------------------------

void CuOptSolverBackend::add_row(int num_elements,
                                 const int* columns,
                                 const double* elements,
                                 double rowlb,
                                 double rowub)
{
  std::map<int, double> entries;
  for (int k = 0; k < num_elements; ++k) {
    entries[columns[k]] = elements[k];
  }
  m_model_.row_entries.push_back(std::move(entries));
  m_model_.row_lb.push_back(rowlb);
  m_model_.row_ub.push_back(rowub);
  ++m_model_.num_rows;
}

void CuOptSolverBackend::add_rows(int num_rows,
                                  const int* rowbeg,
                                  const int* rowind,
                                  const double* rowval,
                                  const double* rowlb,
                                  const double* rowub)
{
  for (int r = 0; r < num_rows; ++r) {
    std::map<int, double> entries;
    for (int k = rowbeg[r]; k < rowbeg[r + 1]; ++k) {
      entries[rowind[k]] = rowval[k];
    }
    m_model_.row_entries.push_back(std::move(entries));
    m_model_.row_lb.push_back(rowlb[r]);
    m_model_.row_ub.push_back(rowub[r]);
  }
  m_model_.num_rows += num_rows;
}

void CuOptSolverBackend::set_row_lower(int index, double value)
{
  m_model_.row_lb[static_cast<size_t>(index)] = value;
}

void CuOptSolverBackend::set_row_upper(int index, double value)
{
  m_model_.row_ub[static_cast<size_t>(index)] = value;
}

void CuOptSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  m_model_.row_lb[static_cast<size_t>(index)] = lb;
  m_model_.row_ub[static_cast<size_t>(index)] = ub;
}

void CuOptSolverBackend::delete_rows(int num, const int* indices)
{
  // Mark-and-compact: collect the row indices to drop (sorted desc so
  // erase does not shift not-yet-erased indices), then erase from the
  // parallel row vectors.  Matches OSI/HiGHS delete_rows semantics.
  std::vector<int> drop(indices, indices + num);
  std::sort(drop.begin(), drop.end(), std::greater<>());
  drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
  for (const int r : drop) {
    const auto ur = static_cast<size_t>(r);
    m_model_.row_entries.erase(m_model_.row_entries.begin()
                               + static_cast<std::ptrdiff_t>(ur));
    m_model_.row_lb.erase(m_model_.row_lb.begin()
                          + static_cast<std::ptrdiff_t>(ur));
    m_model_.row_ub.erase(m_model_.row_ub.begin()
                          + static_cast<std::ptrdiff_t>(ur));
  }
  m_model_.num_rows -= static_cast<int>(drop.size());
}

// ---- coefficients ---------------------------------------------------------

double CuOptSolverBackend::get_coeff(int row, int col) const
{
  const auto& entries = m_model_.row_entries[static_cast<size_t>(row)];
  const auto it = entries.find(col);
  return it != entries.end() ? it->second : 0.0;
}

void CuOptSolverBackend::set_coeff(int row, int col, double value)
{
  // CRITICAL: gtopt's SystemLP::update_lp gates ALL its volume-dependent
  // coefficient updates on supports_set_coeff() == true.  We buffer the
  // override straight into the row's sparse map so the next rebuild picks
  // it up — hence supports_set_coeff() returns true below.
  m_model_.row_entries[static_cast<size_t>(row)][col] = value;
}

bool CuOptSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

// ---- variable types -------------------------------------------------------

void CuOptSolverBackend::set_continuous(int index)
{
  m_model_.col_type[static_cast<size_t>(index)] = CUOPT_CONTINUOUS;
}

void CuOptSolverBackend::set_integer(int index)
{
  m_model_.col_type[static_cast<size_t>(index)] = CUOPT_INTEGER;
}

bool CuOptSolverBackend::is_continuous(int index) const
{
  return m_model_.col_type[static_cast<size_t>(index)] == CUOPT_CONTINUOUS;
}

bool CuOptSolverBackend::is_integer(int index) const
{
  return m_model_.col_type[static_cast<size_t>(index)] == CUOPT_INTEGER;
}

// ---- solution access ------------------------------------------------------
// Bound/obj getters return into the live model; solution getters into the
// post-solve snapshot.  All are stable host memory.

const double* CuOptSolverBackend::col_lower() const
{
  return m_model_.col_lb.data();
}

const double* CuOptSolverBackend::col_upper() const
{
  return m_model_.col_ub.data();
}

const double* CuOptSolverBackend::obj_coefficients() const
{
  return m_model_.col_obj.data();
}

const double* CuOptSolverBackend::row_lower() const
{
  return m_model_.row_lb.data();
}

const double* CuOptSolverBackend::row_upper() const
{
  return m_model_.row_ub.data();
}

const double* CuOptSolverBackend::col_solution() const
{
  return m_sol_.primal.empty() ? nullptr : m_sol_.primal.data();
}

const double* CuOptSolverBackend::reduced_cost() const
{
  return m_sol_.reduced.empty() ? nullptr : m_sol_.reduced.data();
}

const double* CuOptSolverBackend::row_price() const
{
  return m_sol_.dual.empty() ? nullptr : m_sol_.dual.data();
}

double CuOptSolverBackend::obj_value() const
{
  return m_sol_.obj;
}

// ---- solution hints (no-op: cuOpt rebuilds cold every solve) --------------

void CuOptSolverBackend::set_col_solution(const double* /*sol*/) {}
void CuOptSolverBackend::set_row_price(const double* /*price*/) {}

bool CuOptSolverBackend::set_mip_start(const std::span<const double> col_values,
                                       MipStartEffort /*effort*/)
{
  // cuOpt has no persistent settings object (it is created fresh inside
  // solve_()), so buffer the dense start here and replay it via
  // cuOptAddMIPStart on the new settings.  cuOpt has no caller-tunable
  // effort level, so `effort` is advisory.  NOTE: cuOptAddMIPStart is
  // "unsupported with presolve on", so solve_() forces presolve OFF whenever
  // a start is buffered.
  if (col_values.size() != static_cast<std::size_t>(m_model_.num_cols)) {
    m_mip_start_.clear();
    return false;
  }
  m_mip_start_.assign(col_values.begin(), col_values.end());
  return true;
}

// ---- solve ----------------------------------------------------------------

void CuOptSolverBackend::initial_solve()
{
  solve_();
}

void CuOptSolverBackend::resolve()
{
  solve_();
}

SolveEffort CuOptSolverBackend::last_solve_effort() const
{
  return m_last_effort_;
}

void CuOptSolverBackend::solve_()
{
  const int ncols = m_model_.num_cols;
  const int nrows = m_model_.num_rows;

  // 1) Lower the row-major model into cuOpt's CSR ranged-problem arrays.
  std::vector<cuopt_int_t> row_offsets;
  std::vector<cuopt_int_t> col_indices;
  std::vector<cuopt_float_t> coeff_values;
  row_offsets.reserve(static_cast<size_t>(nrows) + 1);
  row_offsets.push_back(0);
  for (int r = 0; r < nrows; ++r) {
    for (const auto& [col, val] : m_model_.row_entries[static_cast<size_t>(r)])
    {
      col_indices.push_back(col);
      coeff_values.push_back(val);
    }
    row_offsets.push_back(static_cast<cuopt_int_t>(col_indices.size()));
  }

  std::vector<cuopt_float_t> con_lb(static_cast<size_t>(nrows));
  std::vector<cuopt_float_t> con_ub(static_cast<size_t>(nrows));
  for (int r = 0; r < nrows; ++r) {
    con_lb[static_cast<size_t>(r)] = to_cuopt_bound(m_model_.row_lb[r]);
    con_ub[static_cast<size_t>(r)] = to_cuopt_bound(m_model_.row_ub[r]);
  }

  std::vector<cuopt_float_t> var_lb(static_cast<size_t>(ncols));
  std::vector<cuopt_float_t> var_ub(static_cast<size_t>(ncols));
  for (int j = 0; j < ncols; ++j) {
    var_lb[static_cast<size_t>(j)] = to_cuopt_bound(m_model_.col_lb[j]);
    var_ub[static_cast<size_t>(j)] = to_cuopt_bound(m_model_.col_ub[j]);
  }

  // 2) Build the immutable problem object.  Ranged form maps gtopt's
  //    (row_lb, row_ub) pair directly — no sense/RHS conversion needed.
  cuOptOptimizationProblem problem = nullptr;
  check(cuOptCreateRangedProblem(nrows,
                                 ncols,
                                 CUOPT_MINIMIZE,
                                 m_model_.obj_offset,
                                 m_model_.col_obj.data(),
                                 row_offsets.data(),
                                 col_indices.data(),
                                 coeff_values.data(),
                                 con_lb.data(),
                                 con_ub.data(),
                                 var_lb.data(),
                                 var_ub.data(),
                                 m_model_.col_type.data(),
                                 &problem),
        "cuOptCreateRangedProblem");

  // 3) Solver settings from the current SolverOptions.
  cuOptSolverSettings settings = nullptr;
  check(cuOptCreateSolverSettings(&settings), "cuOptCreateSolverSettings");

  // Algorithm: gtopt LPAlgo -> cuOpt method.  cuOpt has a true dual-simplex
  // and barrier (cuDSS) in addition to PDLP; barrier+crossover yields clean
  // vertex duals (what gtopt's LMP path wants).
  switch (m_options_.algorithm) {
    case LPAlgo::primal:
      // PDLP: GPU first-order primal-dual. Fast and insensitive to LP
      // conditioning/bound tightness, but returns a non-vertex point
      // (no crossover unless CUOPT_CROSSOVER is set), so exact LMP duals
      // are not guaranteed. Select via `--algorithm primal --solver cuopt`.
      cuOptSetIntegerParameter(settings, CUOPT_METHOD, CUOPT_METHOD_PDLP);
      break;
    case LPAlgo::dual:
      cuOptSetIntegerParameter(
          settings, CUOPT_METHOD, CUOPT_METHOD_DUAL_SIMPLEX);
      break;
    case LPAlgo::barrier:
      cuOptSetIntegerParameter(settings, CUOPT_METHOD, CUOPT_METHOD_BARRIER);
      break;
    case LPAlgo::default_algo:
    case LPAlgo::last_algo:
      cuOptSetIntegerParameter(settings, CUOPT_METHOD, CUOPT_METHOD_CONCURRENT);
      break;
  }
  // Crossover so barrier produces basic (vertex) duals/RC for LMPs.
  cuOptSetIntegerParameter(
      settings, CUOPT_CROSSOVER, m_options_.crossover ? 1 : 0);

  if (const auto tl = m_options_.time_limit; tl && *tl > 0.0) {
    cuOptSetFloatParameter(settings, CUOPT_TIME_LIMIT, *tl);
  }
  if (const auto g = m_options_.mip_gap; g && *g > 0.0) {
    cuOptSetFloatParameter(settings, CUOPT_MIP_RELATIVE_GAP, *g);
  }
  if (const auto ga = m_options_.mip_gap_abs; ga && *ga > 0.0) {
    cuOptSetFloatParameter(settings, CUOPT_MIP_ABSOLUTE_GAP, *ga);
  }
  if (const auto oeps = m_options_.optimal_eps; oeps && *oeps > 0.0) {
    cuOptSetFloatParameter(settings, CUOPT_RELATIVE_GAP_TOLERANCE, *oeps);
  }
  if (const auto feps = m_options_.feasible_eps; feps && *feps > 0.0) {
    cuOptSetFloatParameter(settings, CUOPT_ABSOLUTE_PRIMAL_TOLERANCE, *feps);
  }
  // cuOptAddMIPStart is "unsupported with presolve on", so force presolve OFF
  // whenever a MIP start is buffered (and warn if the user had asked for it).
  const bool has_mip_start = !m_mip_start_.empty();
  if (has_mip_start && m_options_.presolve) {
    spdlog::warn(
        "cuOpt: forcing presolve OFF because a MIP start is set "
        "(cuOptAddMIPStart is unsupported with presolve on)");
  }
  cuOptSetIntegerParameter(settings,
                           CUOPT_PRESOLVE,
                           (m_options_.presolve && !has_mip_start)
                               ? CUOPT_PRESOLVE_DEFAULT
                               : CUOPT_PRESOLVE_OFF);
  if (has_mip_start) {
    check(cuOptAddMIPStart(settings,
                           m_mip_start_.data(),
                           static_cast<cuopt_int_t>(m_mip_start_.size())),
          "cuOptAddMIPStart");
    spdlog::debug("cuOpt: applied MIP start ({} vars)", m_mip_start_.size());
  }
  cuOptSetIntegerParameter(
      settings, CUOPT_LOG_TO_CONSOLE, m_options_.log_level > 0 ? 1 : 0);
  if (m_options_.threads > 0) {
    cuOptSetIntegerParameter(
        settings, CUOPT_NUM_CPU_THREADS, m_options_.threads);
  }

  // File-based logging: when the framework requested ``log_mode=detailed`` it
  // calls ``set_log_filename`` with a per-(scene/phase) stem; route cuOpt's
  // solve log there via the ``CUOPT_LOG_FILE`` string parameter, mirroring the
  // CPLEX backend's per-solve ``.log`` file.
  if (!m_log_filename_.empty()) {
    cuOptSetParameter(settings, CUOPT_LOG_FILE, m_log_filename_.c_str());
  }

  // 3b) Optional user parameter file: ``<solvers>/cuopt.prm``.  gtopt's
  // ``param_file`` is solver-agnostic and the case typically pins it to the
  // CPLEX ``solvers/cplex.prm`` (whose ``CPXPARAM_*`` keys are meaningless to
  // cuOpt), so we read the ``cuopt.prm`` SIBLING of that path instead —
  // mirroring how the CPLEX plugin reads its ``cplex_warmstart.prm`` sibling.
  // Each non-blank, non-comment line is ``<name> <value>`` and is applied
  // LAST via cuOpt's string-keyed ``cuOptSetParameter`` so file values
  // OVERRIDE every gtopt-set default above (e.g. ``method 1`` for PDLP,
  // ``relative_gap_tolerance 1e-4`` to hit the fast first-order regime).
  // ``#``/``//`` lines and blanks are skipped; an unknown key logs a warning
  // but does not abort the solve.
  if (m_options_.param_file.has_value() && !m_options_.param_file->empty()) {
    std::error_code ec;
    const std::filesystem::path prm_path =
        std::filesystem::path {*m_options_.param_file}.parent_path()
        / "cuopt.prm";
    if (std::filesystem::exists(prm_path, ec) && !ec) {
      std::ifstream prm {prm_path};
      std::string line;
      while (std::getline(prm, line)) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#'
            || line.compare(first, 2, "//") == 0)
        {
          continue;
        }
        std::istringstream iss {line.substr(first)};
        std::string name;
        std::string value;
        if (iss >> name >> value) {
          if (cuOptSetParameter(settings, name.c_str(), value.c_str())
              != CUOPT_SUCCESS)
          {
            spdlog::warn(
                "cuOpt: cuopt.prm parameter '{}' = '{}' was rejected "
                "(unknown key or bad value)",
                name,
                value);
          }
        }
      }
    }
  }

  // 4) Solve on the GPU.
  cuOptSolution solution = nullptr;
  const cuopt_int_t solve_status = cuOptSolve(problem, settings, &solution);

  // 5) Snapshot termination + primal/dual/RC/obj into host memory, so the
  //    cuOpt objects can be destroyed before the getters are read.
  m_sol_ = CuOptSolutionCache {};
  m_last_effort_ = SolveEffort {};
  if (solve_status == CUOPT_SUCCESS && solution != nullptr) {
    cuopt_int_t term = CUOPT_TERMINATION_STATUS_NO_TERMINATION;
    cuOptGetTerminationStatus(solution, &term);
    m_sol_.termination = term;

    // GPU solve time (cuOpt has no deterministic work unit → ticks=time).
    cuopt_float_t solve_time = 0.0;
    cuOptGetSolveTime(solution, &solve_time);
    m_last_effort_ = SolveEffort {.seconds = solve_time, .ticks = solve_time};

    cuopt_float_t obj = 0.0;
    cuOptGetObjectiveValue(solution, &obj);
    m_sol_.obj = obj;

    m_sol_.primal.assign(static_cast<size_t>(ncols), 0.0);
    cuOptGetPrimalSolution(solution, m_sol_.primal.data());

    // Duals / reduced costs are LP-only.  For a MIP, cuOpt does not return
    // meaningful duals; gtopt's default fix_mip_and_resolve_duals path pins
    // the integers and re-solves the LP, which routes back through solve_()
    // with a continuous model — so these reads are valid on that pass.
    cuopt_int_t is_mip = 0;
    cuOptIsMIP(problem, &is_mip);
    if (is_mip == 0) {
      m_sol_.dual.assign(static_cast<size_t>(nrows), 0.0);
      cuOptGetDualSolution(solution, m_sol_.dual.data());
      m_sol_.reduced.assign(static_cast<size_t>(ncols), 0.0);
      cuOptGetReducedCosts(solution, m_sol_.reduced.data());
    }
    m_sol_.solved = true;
  }

  // 6) Tear down the immutable objects (cuOpt has no reuse path).
  if (solution != nullptr) {
    cuOptDestroySolution(&solution);
  }
  cuOptDestroySolverSettings(&settings);
  cuOptDestroyProblem(&problem);
}

// ---- robust-solve mode ----------------------------------------------------

void CuOptSolverBackend::engage_robust_solve()
{
  if (!m_saved_robust_options_) {
    m_saved_robust_options_ = m_options_;
  }
  // Loosen tolerances 10x and force a numerically-conservative method.
  auto loosen = [](std::optional<double>& eps, double factor)
  {
    if (eps && *eps > 0.0) {
      *eps *= factor;
    } else {
      eps = 1e-5 * factor;
    }
  };
  loosen(m_options_.optimal_eps, 10.0);
  loosen(m_options_.feasible_eps, 10.0);
  m_options_.algorithm = LPAlgo::barrier;
  m_options_.crossover = true;
}

void CuOptSolverBackend::disengage_robust_solve() noexcept
{
  if (m_saved_robust_options_) {
    m_options_ = *m_saved_robust_options_;
    m_saved_robust_options_.reset();
  }
}

// ---- status ---------------------------------------------------------------

bool CuOptSolverBackend::is_proven_optimal() const
{
  return m_sol_.solved
      && m_sol_.termination == CUOPT_TERMINATION_STATUS_OPTIMAL;
}

bool CuOptSolverBackend::is_abandoned() const
{
  return m_sol_.solved
      && (m_sol_.termination == CUOPT_TERMINATION_STATUS_NUMERICAL_ERROR
          || m_sol_.termination == CUOPT_TERMINATION_STATUS_TIME_LIMIT
          || m_sol_.termination == CUOPT_TERMINATION_STATUS_ITERATION_LIMIT);
}

bool CuOptSolverBackend::is_proven_primal_infeasible() const
{
  return m_sol_.solved
      && (m_sol_.termination == CUOPT_TERMINATION_STATUS_INFEASIBLE
          || m_sol_.termination
              == CUOPT_TERMINATION_STATUS_UNBOUNDED_OR_INFEASIBLE);
}

bool CuOptSolverBackend::is_proven_dual_infeasible() const
{
  return m_sol_.solved
      && m_sol_.termination == CUOPT_TERMINATION_STATUS_UNBOUNDED;
}

// ---- options --------------------------------------------------------------

void CuOptSolverBackend::apply_options(const SolverOptions& opts)
{
  m_options_ = opts;
}

SolverOptions CuOptSolverBackend::optimal_options() const
{
  // GPU PDLP/barrier is the strength; default to barrier + crossover so
  // gtopt's LMP/dual consumers get vertex duals.  Users override via JSON.
  SolverOptions opts {};
  opts.algorithm = LPAlgo::barrier;
  opts.crossover = true;
  return opts;
}

LPAlgo CuOptSolverBackend::get_algorithm() const
{
  return m_options_.algorithm;
}

int CuOptSolverBackend::get_threads() const
{
  return m_options_.threads;
}

bool CuOptSolverBackend::get_presolve() const
{
  return m_options_.presolve;
}

int CuOptSolverBackend::get_log_level() const
{
  return m_options_.log_level;
}

// ---- diagnostics ----------------------------------------------------------

std::optional<double> CuOptSolverBackend::get_kappa() const
{
  // cuOpt does not expose a basis condition number via the C API.
  return std::nullopt;
}

// ---- logging --------------------------------------------------------------

void CuOptSolverBackend::open_log(FILE* /*file*/, int level)
{
  m_options_.log_level = level;
}

void CuOptSolverBackend::close_log()
{
  m_options_.log_level = 0;
}

void CuOptSolverBackend::set_log_filename(const std::string& filename,
                                          int level)
{
  // Mirror the CPLEX backend: enable file logging only when the caller wants
  // it (level > 0), so we never drop a stray log file otherwise.  The path is
  // applied to ``CUOPT_LOG_FILE`` in ``solve_()`` (the cuOpt settings object
  // lives there); cuOpt writes its solve log to that file.
  if (level > 0 && !filename.empty()) {
    m_log_filename_ = filename + ".log";
    m_options_.log_level = level;
  }
}

void CuOptSolverBackend::clear_log_filename()
{
  m_log_filename_.clear();
}

// ---- names & LP output ----------------------------------------------------

void CuOptSolverBackend::push_names(
    const std::vector<std::string>& /*col_names*/,
    const std::vector<std::string>& /*row_names*/)
{
  // cuOpt's C API problem object carries no column/row names; the MPS
  // writer auto-generates positional names.  No-op (names are a debug aid).
}

void CuOptSolverBackend::write_lp(const char* filename)
{
  // cuOpt only writes MPS (CUOPT_FILE_FORMAT_MPS).  Build a throwaway
  // problem from the current model and dump it.
  // TODO(cuopt): wire cuOptWriteProblem(problem, filename,
  // CUOPT_FILE_FORMAT_MPS)
  //              once a long-lived problem handle is retained; for now this
  //              is a debug-only path and left as a no-op stub.
  (void)filename;
}

// ---- clone ----------------------------------------------------------------

std::unique_ptr<SolverBackend> CuOptSolverBackend::clone() const
{
  auto copy = std::make_unique<CuOptSolverBackend>();
  copy->m_model_ = m_model_;  // deep copy of the mutable host model
  copy->m_sol_ = m_sol_;
  copy->m_options_ = m_options_;
  copy->m_prob_name_ = m_prob_name_;
  return copy;
}

}  // namespace gtopt

// ===========================================================================
// SCAFFOLD — remaining implementation / validation steps (none risk a
// half-broken build; the plugin compiles + links, but is not yet validated
// against the full gtopt LP pipeline or a live GPU solve):
//
//  S1. Objective offset wiring.  gtopt feeds the FCF/scale offset through
//      set_obj_coeff on a dedicated constant column today, not a problem
//      offset; confirm whether any caller expects obj_offset and remove the
//      unused field, or wire it from the planning layer.  (Currently 0.)
//
//  S2. MIP duals.  Verify gtopt's default fix_mip_and_resolve_duals path
//      (base class) round-trips correctly here: it set_col_lower/upper-pins
//      the integers, relax_all_integers() (default loops set_continuous),
//      apply_options, resolve() — all of which we buffer + rebuild, so the
//      follow-up solve is a pure-LP rebuild that DOES return duals/RC.
//      Add a doctest asserting duals are populated on that second pass.
//
//  S3. Status mapping.  Confirm CUOPT_TERMINATION_STATUS_* → gtopt's
//      is_proven_* against cuOpt's actual codes for marginal cases
//      (PRIMAL_FEASIBLE / FEASIBLE_FOUND on an aborted MIP). gtopt treats
//      "not optimal" as a fallback trigger; map FEASIBLE_FOUND to "optimal
//      enough" only if a MIP gap target was met.
//
//  S4. Rebuild cost.  Every resolve() re-transposes + re-uploads the full
//      matrix to the GPU.  For SDDP (thousands of resolves per phase on a
//      mostly-static matrix) this is the dominant cost.  Optimization:
//      cache the lowered CSR (row_offsets/col_indices/coeff_values) and only
//      patch changed entries; track a dirty flag per mutator.  Functionally
//      correct without this; needed for it to be FASTER than CPU CPLEX.
//
//  S5. GPU memory ceiling.  A 1.6M-var / multi-M-row LP at FP64 may exceed
//      the RTX 5060's VRAM.  Add a try/catch around cuOptSolve mapping
//      CUOPT_OUT_OF_MEMORY → is_abandoned() so gtopt's algorithm-fallback
//      chain degrades gracefully (or fails over to a CPU solver) rather
//      than aborting.
//
//  S6. delete_rows + cut indices.  gtopt's SDDP cut store calls delete_rows
//      with absolute row indices; our compact-erase keeps row order, which
//      matches OSI/HiGHS.  Add a doctest mirroring test_*delete_rows* to
//      confirm row-index stability across add_row/delete_rows interleaving.
//
//  S7. set_col_bounds_bulk / set_row_bounds_bulk / set_obj_coeffs.  We
//      inherit the base-class per-element fallbacks (correct).  Override
//      with direct vector writes for speed once S4 caching lands.
//
//  S8. Concurrency.  cuOpt uses a single default GPU stream; gtopt's
//      WorkPool runs many clones in parallel.  Confirm thread-safety of
//      concurrent cuOptSolve calls (likely needs a per-thread settings obj
//      and/or a GPU serialization mutex). Until verified, gate cuOpt to the
//      coordinator/serial solve tiers.
// ===========================================================================
