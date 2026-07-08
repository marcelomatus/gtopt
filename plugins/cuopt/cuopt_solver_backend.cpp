/**
 * @file      cuopt_solver_backend.cpp
 * @brief     NVIDIA cuOpt (GPU) solver backend implementation
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
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cuopt_solver_backend.hpp"

// cuOpt ≥ 25.12 renamed the C-API include tree from linear_programming/ to
// mathematical_optimization/ (the old cuopt_c.h remains as a deprecated
// shim, constants.h was removed outright); support both layouts.
#if __has_include(<cuopt/mathematical_optimization/cuopt_c.h>)
#  include <cuopt/mathematical_optimization/constants.h>
#  include <cuopt/mathematical_optimization/cuopt_c.h>
#else
#  include <cuopt/linear_programming/constants.h>
#  include <cuopt/linear_programming/cuopt_c.h>
#endif
#include <gtopt/log_and_throw.hpp>
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
    log_and_throw(std::format("cuopt: {} failed with status {}", what, status));
  }
}

/// Process-global serialization mutex for every cuOpt C-API interaction.
///
/// cuOpt's library-global state is NOT thread-safe: its `rapids_logger`
/// singleton and the lazy cuBLAS/raft handle init are shared across threads.
/// gtopt's SDDP work pool dispatches multiple scene solves concurrently, so
/// two overlapping `cuOptSolve`/`cuOptSet*` calls race — observed as either a
/// pure-virtual abort (the logger's sink vtable mutated mid-call) or a hang
/// (two threads in `cublasLtCtxInit` at once).  The single GPU time-slices
/// kernels across contexts in Default compute mode, so serializing solves
/// costs no real throughput.  See TODO(cuopt) S8 at the bottom of this file.
///
/// MULTI-GPU: this lock is *process-global* by design, not per-device, and is
/// correct on any GPU count.  The `rapids_logger` singleton (root cause #1)
/// lives in host memory and is shared across every device, so a per-GPU mutex
/// would still race on it — a global lock is mandatory for correctness.  The
/// trade-off is that it also serializes the GPU-compute portion across all
/// devices, so it does NOT exploit multi-GPU parallelism.  That is acceptable
/// today because this backend has no device-assignment logic (every
/// `cuOptSolve` targets the default CUDA device).  True multi-GPU *scaling*
/// requires per-instance device pinning (`cudaSetDevice`/CUDA_VISIBLE_DEVICES)
/// plus a two-tier lock — a process-global lock only around the logger/settings
/// init and a per-device lock around the solve — which is deferred to the
/// GPU-aware work-pool work (see the resource-manager proposal, phase P1).
std::mutex g_cuopt_global_mutex;

/// Host-side CSR lowering of a `CuOptModel`, ready for
/// `cuOptCreateRangedProblem`.  Pure host work — build it OUTSIDE
/// `g_cuopt_global_mutex` so it can overlap another thread's GPU solve.
struct LoweredModel
{
  std::vector<cuopt_int_t> row_offsets {};
  std::vector<cuopt_int_t> col_indices {};
  std::vector<cuopt_float_t> coeff_values {};
  std::vector<cuopt_float_t> con_lb {};
  std::vector<cuopt_float_t> con_ub {};
  std::vector<cuopt_float_t> var_lb {};
  std::vector<cuopt_float_t> var_ub {};
};

[[nodiscard]] LoweredModel lower_model(const CuOptModel& model)
{
  const int ncols = model.num_cols;
  const int nrows = model.num_rows;

  LoweredModel lm;
  lm.row_offsets.reserve(static_cast<size_t>(nrows) + 1);
  lm.row_offsets.push_back(0);
  for (int r = 0; r < nrows; ++r) {
    for (const auto& [col, val] : model.row_entries[static_cast<size_t>(r)]) {
      lm.col_indices.push_back(col);
      lm.coeff_values.push_back(val);
    }
    lm.row_offsets.push_back(static_cast<cuopt_int_t>(lm.col_indices.size()));
  }

  lm.con_lb.resize(static_cast<size_t>(nrows));
  lm.con_ub.resize(static_cast<size_t>(nrows));
  for (int r = 0; r < nrows; ++r) {
    lm.con_lb[static_cast<size_t>(r)] = to_cuopt_bound(model.row_lb[r]);
    lm.con_ub[static_cast<size_t>(r)] = to_cuopt_bound(model.row_ub[r]);
  }

  lm.var_lb.resize(static_cast<size_t>(ncols));
  lm.var_ub.resize(static_cast<size_t>(ncols));
  for (int j = 0; j < ncols; ++j) {
    lm.var_lb[static_cast<size_t>(j)] = to_cuopt_bound(model.col_lb[j]);
    lm.var_ub[static_cast<size_t>(j)] = to_cuopt_bound(model.col_ub[j]);
  }
  return lm;
}

/// Build the immutable cuOpt problem object from a lowered model.  Ranged
/// form maps gtopt's (row_lb, row_ub) pair directly — no sense/RHS
/// conversion.  Caller must hold `g_cuopt_global_mutex` and destroy the
/// returned problem via `cuOptDestroyProblem`.
[[nodiscard]] cuOptOptimizationProblem create_ranged_problem(
    const CuOptModel& model, const LoweredModel& lm)
{
  cuOptOptimizationProblem problem = nullptr;
  check(cuOptCreateRangedProblem(model.num_rows,
                                 model.num_cols,
                                 CUOPT_MINIMIZE,
                                 model.obj_offset,
                                 model.col_obj.data(),
                                 lm.row_offsets.data(),
                                 lm.col_indices.data(),
                                 lm.coeff_values.data(),
                                 lm.con_lb.data(),
                                 lm.con_ub.data(),
                                 lm.var_lb.data(),
                                 lm.var_ub.data(),
                                 model.col_type.data(),
                                 &problem),
        "cuOptCreateRangedProblem");
  return problem;
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
  const std::scoped_lock cuopt_guard(g_cuopt_global_mutex);
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
  // A fresh model invalidates every solution-derived hint: the previous
  // snapshot (cleared above) and any explicit warm-start buffers, which
  // were sized/indexed against the OLD column/row layout.
  m_warm_primal_.clear();
  m_warm_dual_.clear();
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

void CuOptSolverBackend::set_obj_offset(double raw_offset) noexcept
{
  // ABSOLUTE set.  The host model carries the constant into
  // `cuOptCreateRangedProblem` (`objective_offset`) at solve time, and
  // `clone()` deep-copies m_model_ — so storing it here is sufficient.  The
  // LinearInterface re-calls this after every load_problem (which resets
  // m_model_), keeping the offset current across reconstructs.
  m_model_.obj_offset = raw_offset;
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

// ---- solution hints (vector warm start) ------------------------------------
//
// cuOpt's C API accepts an initial primal/dual pair (PDLP restart) via
// `cuOptSetInitialPrimalSolution` / `cuOptSetInitialDualSolution` on the
// settings object.  These buffers hold EXPLICIT caller hints; when absent,
// `solve_()` falls back to auto-seeding from the previous optimal snapshot.
// The caller passes bare pointers (OSI convention), so the size is taken
// from the CURRENT model dimensions at buffering time.

void CuOptSolverBackend::set_col_solution(const double* sol)
{
  if (sol == nullptr || m_model_.num_cols <= 0) {
    m_warm_primal_.clear();
    return;
  }
  m_warm_primal_.assign(sol, sol + m_model_.num_cols);
}

void CuOptSolverBackend::set_row_price(const double* price)
{
  if (price == nullptr || m_model_.num_rows <= 0) {
    m_warm_dual_.clear();
    return;
  }
  m_warm_dual_.assign(price, price + m_model_.num_rows);
}

bool CuOptSolverBackend::set_mip_start(const std::span<const double> col_values,
                                       MipStartEffort effort)
{
  // cuOpt has no persistent settings object (it is created fresh inside
  // solve_()), so buffer the dense start + effort here and replay them via
  // cuOptAddMIPStart on the new settings.  NOTE: cuOptAddMIPStart is
  // "unsupported with presolve on" (still true in 26.06), so solve_() forces
  // presolve OFF whenever a start is buffered.  `effort` is accepted but not
  // forwarded: cuOpt repairs every injected start natively — its
  // add_user_given_solutions() fixes the start's integer values and
  // LP-solves the continuous part to first-feasible before seeding the
  // heuristic population — so repair/solve_fixed semantics are built in.
  if (col_values.size() != static_cast<std::size_t>(m_model_.num_cols)) {
    m_mip_start_.clear();
    return false;
  }
  m_mip_start_.assign(col_values.begin(), col_values.end());
  m_mip_start_effort_ = effort;
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
  const auto lowered = lower_model(m_model_);

  // Serialize every cuOpt C-API interaction below: the library's global
  // logger/cuBLAS state is not thread-safe and the SDDP work pool drives
  // several scene solves concurrently (see g_cuopt_global_mutex).  The
  // host-side CSR lowering above is per-instance and stays outside the lock
  // so it can overlap another thread's GPU solve.
  const std::scoped_lock cuopt_guard(g_cuopt_global_mutex);

  // 2) Build the immutable problem object.
  cuOptOptimizationProblem problem = create_ranged_problem(m_model_, lowered);

  // 3) Solver settings from the current SolverOptions.
  cuOptSolverSettings settings = nullptr;
  check(cuOptCreateSolverSettings(&settings), "cuOptCreateSolverSettings");

  // Algorithm: gtopt LPAlgo -> cuOpt method.  cuOpt has a true dual-simplex
  // and barrier (cuDSS) in addition to PDLP; barrier+crossover yields clean
  // vertex duals (what gtopt's LMP path wants).
  cuopt_int_t cuopt_method = CUOPT_METHOD_CONCURRENT;
  switch (m_options_.algorithm) {
    case LPAlgo::primal:
      // "primal" maps to CONCURRENT, NOT lone PDLP.  Lone PDLP (GPU
      // first-order) has two failure modes that break gtopt: (1) it wedges
      // the WSL2 CUDA driver in its per-iteration device->host convergence
      // poll (unrecoverable hang — see g_cuopt_global_mutex / TODO S8), and
      // (2) its infeasibility detection is unreliable, so it falsely reports
      // tight-but-feasible LPs (e.g. an SDDP terminal phase with a hard
      // reservoir target) as INFEASIBLE.  CONCURRENT still runs PDLP (and
      // returns it when it wins the race) but always has a simplex leg that
      // terminates with a correct vertex verdict.  Force true lone PDLP for
      // benchmarking via the GTOPT_CUOPT_METHOD=pdlp env override below.
      cuopt_method = CUOPT_METHOD_CONCURRENT;
      break;
    case LPAlgo::dual:
      cuopt_method = CUOPT_METHOD_DUAL_SIMPLEX;
      break;
    case LPAlgo::barrier:
      // "barrier" also maps to CONCURRENT, NOT lone BARRIER: cuOpt 26.08's
      // cuDSS-based barrier wedges in cuStreamSynchronize inside the sparse
      // Cholesky solve on WSL2 (observed on the algorithm-fallback tests;
      // core backtrace: barrier_solver_t::gpu_compute_search_direction ->
      // sparse_cholesky_cudss_t::solve -> cuStreamSynchronize, unrecoverable).
      // CONCURRENT keeps a simplex leg that terminates.  Force true lone
      // barrier via GTOPT_CUOPT_METHOD=barrier below.
      cuopt_method = CUOPT_METHOD_CONCURRENT;
      break;
    case LPAlgo::default_algo:
    case LPAlgo::last_algo:
      cuopt_method = CUOPT_METHOD_CONCURRENT;
      break;
  }
  // Optional runtime override: GTOPT_CUOPT_METHOD in
  // {pdlp, dual, barrier, concurrent}.  Provided as a triage lever and a WSL2
  // robustness escape hatch — PDLP's per-iteration device->host convergence
  // poll (`get_norm_squared_delta_dual` -> `cuMemcpyAsync`) can wedge the WSL
  // CUDA driver on some LPs, and a direct method (barrier/dual/concurrent)
  // sidesteps that poll.
  if (const char* env = std::getenv("GTOPT_CUOPT_METHOD"); env != nullptr) {
    const std::string_view m {env};
    if (m == "pdlp" || m == "primal") {
      cuopt_method = CUOPT_METHOD_PDLP;
    } else if (m == "dual") {
      cuopt_method = CUOPT_METHOD_DUAL_SIMPLEX;
    } else if (m == "barrier") {
      cuopt_method = CUOPT_METHOD_BARRIER;
    } else if (m == "concurrent") {
      cuopt_method = CUOPT_METHOD_CONCURRENT;
    }
  }
  cuOptSetIntegerParameter(settings, CUOPT_METHOD, cuopt_method);
  // Crossover so barrier produces basic (vertex) duals/RC for LMPs.  cuOpt
  // has no primal/dual selector, so any non-none mode enables crossover.
  cuOptSetIntegerParameter(settings,
                           CUOPT_CROSSOVER,
                           m_options_.crossover == CrossoverMode::none ? 0 : 1);

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

  // ---- vector warm start (LP only) ----------------------------------------
  //
  // Seed the solve with an initial primal/dual pair — cuOpt's PDLP restart.
  // Source priority: explicit hints buffered by `set_col_solution` /
  // `set_row_price`, else the previous OPTIMAL solution snapshot from this
  // instance (or from the clone parent — `clone()` copies `m_sol_`), which is
  // exactly the SDDP incremental-re-solve shape: same LP plus a new cut row
  // and/or moved bounds.  The dual is zero-padded for rows appended since the
  // snapshot (a fresh cut row starts with dual 0); if rows were DELETED the
  // old dual indexing is unrecoverable, so the dual hint is skipped (the
  // primal hint alone is still valid and useful).
  //
  // MIP solves are excluded: initial primal/dual is a PDLP (LP) mechanism;
  // the MIP path uses `cuOptAddMIPStart`.
  //
  // Presolve is forced OFF when a start is seeded: measured on this repo's
  // LPs (2026-07-07, RTX 5060), a warm start under PAPILO or PSLP is wasted
  // (≈cold time or slower), while with presolve off it is 5-7x at the
  // unchanged optimum and ~1.7x after an SDDP-aperture-like perturbation.
  // The 26.02 release notes made the same point ("to use PDLP warm start,
  // presolve must now be explicitly disabled").
  //
  // Kill switch: GTOPT_CUOPT_WARMSTART=0 (triage lever, mirrors
  // GTOPT_CUOPT_METHOD).
  bool is_mip_model = false;
  for (const char t : m_model_.col_type) {
    if (t != CUOPT_CONTINUOUS) {
      is_mip_model = true;
      break;
    }
  }
  bool warm_enabled = true;
  if (const char* env = std::getenv("GTOPT_CUOPT_WARMSTART");
      env != nullptr && env[0] == '0')
  {
    warm_enabled = false;
  }
  bool warm_seeded = false;
  // Lifetime: the Set-initial-* docs only promise the pointers refer to
  // host memory, not that the values are copied at Set time — so every
  // seeded buffer must outlive `cuOptSolve` below.  The explicit hints are
  // MOVED into these solve-scoped locals (making them one-shot: consumed
  // here, later solves fall back to the auto snapshot); `m_sol_.*` is a
  // member reset only after the solve; the zero-padded dual gets its own
  // solve-scoped holder.
  const std::vector<double> explicit_primal = std::move(m_warm_primal_);
  const std::vector<double> explicit_dual = std::move(m_warm_dual_);
  m_warm_primal_.clear();  // normalize moved-from state
  m_warm_dual_.clear();
  std::vector<double> padded_dual;
  if (warm_enabled && !is_mip_model) {
    const auto un = static_cast<size_t>(ncols);
    const auto um = static_cast<size_t>(nrows);
    // Primal hint: explicit buffer first, else previous optimal snapshot.
    const std::vector<double>* primal_src = nullptr;
    const std::vector<double>* dual_src = nullptr;
    if (explicit_primal.size() == un) {
      primal_src = &explicit_primal;
      dual_src = explicit_dual.empty() ? nullptr : &explicit_dual;
    } else if (m_sol_.solved
               && m_sol_.termination == CUOPT_TERMINATION_STATUS_OPTIMAL
               && m_sol_.primal.size() == un)
    {
      primal_src = &m_sol_.primal;
      dual_src = m_sol_.dual.empty() ? nullptr : &m_sol_.dual;
    }
    if (primal_src != nullptr) {
      if (cuOptSetInitialPrimalSolution(
              settings, primal_src->data(), static_cast<cuopt_int_t>(un))
          == CUOPT_SUCCESS)
      {
        warm_seeded = true;
      }
      if (warm_seeded && dual_src != nullptr && dual_src->size() <= um) {
        if (dual_src->size() < um) {
          // Rows appended since the snapshot: pad new rows' duals with 0.
          padded_dual = *dual_src;
          padded_dual.resize(um, 0.0);
          dual_src = &padded_dual;
        }
        cuOptSetInitialDualSolution(
            settings, dual_src->data(), static_cast<cuopt_int_t>(um));
      }
      if (warm_seeded) {
        spdlog::debug(
            "cuOpt: warm start seeded ({} cols, dual {}) — presolve OFF",
            un,
            (dual_src == nullptr)     ? "none"
                : padded_dual.empty() ? "full"
                                      : "padded");
      }
    }
  }

  // Presolver selection.  PSLP (cuOpt's own GPU-side presolver, what
  // CUOPT_PRESOLVE_DEFAULT resolves to for LPs since 26.02) — measured on a
  // real CEN full-network LP (62k cols / 25k rows, 2026-07-08): with
  // crossover on, PSLP 3.3s vs PAPILO 4.0s vs off 6.2s; duals fully
  // postsolved under both.  An earlier cuOpt release could hang after
  // "PSLP declares problem as infeasible" — re-probed on 26.08 with both an
  // infeasible and an unbounded model: PSLP returns the verdict cleanly in
  // ~0.1s (faster than PAPILO or no presolve), so the old PAPILO-forcing
  // workaround is retired.  Presolve is OFF whenever a MIP start or a warm
  // start is active (both are unsupported/wasted under presolve — see above).
  cuOptSetIntegerParameter(
      settings,
      CUOPT_PRESOLVE,
      (m_options_.presolve && !has_mip_start && !warm_seeded)
          ? CUOPT_PRESOLVE_PSLP
          : CUOPT_PRESOLVE_OFF);
  if (has_mip_start) {
    check(cuOptAddMIPStart(settings,
                           m_mip_start_.data(),
                           static_cast<cuopt_int_t>(m_mip_start_.size())),
          "cuOptAddMIPStart");
    spdlog::debug("cuOpt: applied MIP start ({} vars, effort={})",
                  m_mip_start_.size(),
                  static_cast<int>(m_mip_start_effort_));
    // No per-effort parameter: CUOPT_FIRST_PRIMAL_FEASIBLE binds ONLY to
    // pdlp_settings (an LP stop-early knob; the MIP path ignores it), and
    // setting it here would truncate every MIP solve at the first incumbent
    // if a future cuOpt ever honours it MIP-wide.  cuOpt already repairs
    // injected starts natively (see set_mip_start), which covers
    // effort=repair — gtopt's default.
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
  } else {
    // TODO(cuopt) S5 resolved: a failed cuOptSolve — most importantly
    // CUOPT_OUT_OF_MEMORY (VRAM exhaustion on large FP64 LPs) — must read
    // as is_abandoned() so gtopt's algorithm/solver fallback chain engages
    // (retry with another method, or fail over to a CPU backend) instead of
    // an opaque "non-optimal" that would retry-storm the GPU.
    spdlog::warn(
        "cuopt: cuOptSolve failed with status {}{}",
        solve_status,
        solve_status == CUOPT_OUT_OF_MEMORY ? " (GPU out of memory)" : "");
    m_sol_.solved = true;
    m_sol_.termination = CUOPT_TERMINATION_STATUS_NUMERICAL_ERROR;
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
  m_options_.crossover = CrossoverMode::primal;
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
  // Default to CONCURRENT (cuOpt races PDLP + dual-simplex + barrier and
  // returns the first to finish), with crossover so gtopt's LMP/dual
  // consumers still get vertex duals.
  //
  // Why not pure barrier or pure PDLP: on some LPs (e.g. a terminal SDDP
  // phase with a hard reservoir target) cuOpt's barrier reports non-optimal,
  // which trips `LinearInterface::resolve`'s algorithm-fallback cycle
  // (barrier -> dual -> primal).  The primal/PDLP leg then wedges the WSL2
  // CUDA driver in its per-iteration device->host convergence poll (see
  // g_cuopt_global_mutex notes and TODO S8) — an unrecoverable hang.
  // CONCURRENT sidesteps both: it always has a simplex leg that terminates
  // and returns a vertex solution, so no fallback (hence no lone PDLP) is
  // ever needed.  Users can still force a single method via `--algorithm` or
  // the GTOPT_CUOPT_METHOD env override.
  SolverOptions opts {};
  opts.algorithm = LPAlgo::default_algo;
  opts.crossover = CrossoverMode::primal;
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
  // cuOpt only writes MPS (CUOPT_FILE_FORMAT_MPS): build a throwaway
  // problem object from the current host model and dump it.  Debug-only
  // path — a failure logs a warning instead of aborting the run.
  if (filename == nullptr || m_model_.num_cols <= 0) {
    return;
  }
  const auto lowered = lower_model(m_model_);
  const std::scoped_lock cuopt_guard(g_cuopt_global_mutex);
  cuOptOptimizationProblem problem = nullptr;
  try {
    problem = create_ranged_problem(m_model_, lowered);
  } catch (const std::exception& e) {
    spdlog::warn("cuOpt: write_lp('{}') failed to build problem: {}",
                 filename,
                 e.what());
    return;
  }
  // gtopt callers pass an extensionless stem or a ".lp" path; cuOpt's
  // writer emits MPS regardless, so normalise to a ".mps" suffix to keep
  // the on-disk format honest.
  std::filesystem::path out {filename};
  if (out.extension() != ".mps") {
    out.replace_extension(".mps");
  }
  if (cuOptWriteProblem(problem, out.string().c_str(), CUOPT_FILE_FORMAT_MPS)
      != CUOPT_SUCCESS)
  {
    spdlog::warn("cuOpt: cuOptWriteProblem('{}') failed", out.string());
  }
  cuOptDestroyProblem(&problem);
}

// ---- clone ----------------------------------------------------------------

std::unique_ptr<SolverBackend> CuOptSolverBackend::clone() const
{
  auto copy = std::make_unique<CuOptSolverBackend>();
  copy->m_model_ = m_model_;  // deep copy of the mutable host model
  // Copying the snapshot lets clones (SDDP aperture / forward paths)
  // auto-warm-start their first solve from the parent's optimum.
  copy->m_sol_ = m_sol_;
  copy->m_warm_primal_ = m_warm_primal_;
  copy->m_warm_dual_ = m_warm_dual_;
  copy->m_options_ = m_options_;
  copy->m_prob_name_ = m_prob_name_;
  return copy;
}

}  // namespace gtopt

// ===========================================================================
// Remaining hardening steps.  (Historical S-items already RESOLVED: S5
// failed-solve → is_abandoned() mapping; S8 process-global mutex — see
// g_cuopt_global_mutex.  Shipped 2026-07-08 after empirical GPU validation:
// vector warm start incl. auto-seed + clone inheritance, presolve
// PAPILO→PSLP retirement of the stale infeasibility-hang workaround, and
// write_lp via cuOptWriteProblem.)
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
//  S6. delete_rows + cut indices.  gtopt's SDDP cut store calls delete_rows
//      with absolute row indices; our compact-erase keeps row order, which
//      matches OSI/HiGHS.  Add a doctest mirroring test_*delete_rows* to
//      confirm row-index stability across add_row/delete_rows interleaving.
//
//  S7. set_col_bounds_bulk / set_row_bounds_bulk / set_obj_coeffs.  We
//      inherit the base-class per-element fallbacks (correct).  Override
//      with direct vector writes for speed once S4 caching lands.
//
//  S9. Native-mode PDLP.  With warm start wired, evaluate re-enabling lone
//      PDLP for SDDP re-solves via CUOPT_INFEASIBILITY_DETECTION /
//      CUOPT_STRICT_INFEASIBILITY (added upstream; untested here) — the
//      false-infeasible verdicts were half the reason `primal` maps to
//      CONCURRENT.  The WSL2 convergence-poll wedge needs its own re-probe
//      on current drivers before any default change.
// ===========================================================================
