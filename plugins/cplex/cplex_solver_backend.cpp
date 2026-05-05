/**
 * @file      cplex_solver_backend.cpp
 * @brief     CPLEX C Callable Library solver backend implementation
 * @date      Tue Mar 25 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cplex_solver_backend.hpp"

#include <gtopt/solver_options.hpp>
#include <ilcplex/cplex.h>

namespace gtopt
{

namespace
{

/// Convert ranged-row bounds (lb, ub) to CPLEX sense + rhs + range.
///
/// CPLEX stores rows as   sense  rhs  [rangeval]
///   'L' : row <= rhs
///   'G' : row >= rhs
///   'E' : row == rhs
///   'R' : rhs <= row <= rhs + |rangeval|
///
/// @param lb       Row lower bound (use -cpx_inf for no lower bound)
/// @param ub       Row upper bound (use +cpx_inf for no upper bound)
/// @param cpx_inf  CPLEX infinity constant (CPX_INFBOUND)
/// @param sense    [out] CPLEX constraint sense character
/// @param rhs      [out] CPLEX right-hand-side value
/// @param range    [out] CPLEX range value (only used for 'R' sense)
void bounds_to_cplex(double lb,
                     double ub,
                     double cpx_inf,
                     char& sense,
                     double& rhs,
                     double& range)
{
  const bool lb_inf = (lb <= -cpx_inf);
  const bool ub_inf = (ub >= cpx_inf);

  if (lb_inf && ub_inf) {
    // Free row: -inf <= row <= +inf  →  any sense works; use 'G' with -inf
    sense = 'G';
    rhs = -cpx_inf;
    range = 0.0;
  } else if (lb_inf) {
    sense = 'L';
    rhs = ub;
    range = 0.0;
  } else if (ub_inf) {
    sense = 'G';
    rhs = lb;
    range = 0.0;
  } else if (std::abs(ub - lb) < 1e-12) {
    sense = 'E';
    rhs = lb;
    range = 0.0;
  } else {
    // Ranged row: lb <= row <= ub
    sense = 'R';
    rhs = lb;
    range = ub - lb;
  }
}

/// Recover row lower bound from CPLEX sense/rhs/range.
double cplex_row_lb(char sense, double rhs, double range, double cpx_inf)
{
  switch (sense) {
    case 'L':
      return -cpx_inf;
    case 'G':
    case 'E':
      return rhs;
    case 'R':
      return (range > 0) ? rhs : rhs + range;
    default:
      return -cpx_inf;
  }
}

/// Recover row upper bound from CPLEX sense/rhs/range.
double cplex_row_ub(char sense, double rhs, double range, double cpx_inf)
{
  switch (sense) {
    case 'L':
    case 'E':
      return rhs;
    case 'G':
      return cpx_inf;
    case 'R':
      return rhs + std::abs(range);
    default:
      return cpx_inf;
  }
}

/// Apply a SolverOptions bundle onto a CPLEX env.  Pure env-level mutation
/// — never touches backend member fields.  Shared between CplexEnvLp's
/// ctor (fresh env preparation) and CplexSolverBackend::apply_options()
/// (live in-place tweaks).
void apply_options_to_env(cpxenv* env, const SolverOptions& opts)
{
  if (opts.threads > 0) {
    CPXsetintparam(env, CPX_PARAM_THREADS, opts.threads);
  }

  // Force opportunistic parallel mode (-1, ``CPX_PARALLEL_OPPORTUNISTIC``)
  // by default.  Per the GTEP LP benchmark
  // (``docs/analysis/cplex-benchmark-results.md``) this is "the single
  // best option: 733ms median, tightest range (718-805ms), lowest ticks
  // (1,150).  Free ~3% speedup."  CPLEX's own default is deterministic
  // (``+1``) which serialises some internal phases for run-to-run
  // reproducibility — opportunistic relaxes that and can produce
  // slightly different floating-point results between runs but
  // converges to the same optimum within tolerance.  Acceptable for
  // SDDP because individual cell solves are themselves wrapped in a
  // tolerance-based convergence test, not a bit-exact compare.
  CPXsetintparam(env, CPX_PARAM_PARALLELMODE, -1);

  CPXsetintparam(env, CPX_PARAM_PREIND, opts.presolve ? CPX_ON : CPX_OFF);

  if (opts.scaling.has_value()) {
    int scaind = 0;
    switch (*opts.scaling) {
      case SolverScaling::none:
        scaind = -1;
        break;
      case SolverScaling::automatic:
        scaind = 0;
        break;
      case SolverScaling::aggressive:
        scaind = 1;
        break;
    }
    CPXsetintparam(env, CPX_PARAM_SCAIND, scaind);
  }

  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    CPXsetdblparam(env, CPX_PARAM_EPOPT, *oeps);
  }
  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    CPXsetdblparam(env, CPX_PARAM_EPRHS, *feps);
  }
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    CPXsetdblparam(env, CPX_PARAM_TILIM, *tl);
  }

  // Only touch CPX_PARAM_MEMORYEMPHASIS when the user explicitly set
  // SolverOptions::memory_emphasis — otherwise leave CPLEX's own default.
  if (const auto me = opts.memory_emphasis; me.has_value()) {
    CPXsetintparam(env, CPX_PARAM_MEMORYEMPHASIS, *me ? CPX_ON : CPX_OFF);
  }

  {
    switch (opts.algorithm) {
      case LPAlgo::default_algo:
        CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_AUTOMATIC);
        break;
      case LPAlgo::primal:
        CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_PRIMAL);
        break;
      case LPAlgo::dual:
        CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_DUAL);
        break;
      case LPAlgo::barrier:
        CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_BARRIER);
        if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
          CPXsetdblparam(env, CPX_PARAM_BAREPCOMP, *beps);
        }
        break;
      case LPAlgo::last_algo:
        break;
    }

    // Apply crossover regardless of algorithm: CPLEX may auto-pick
    // barrier under LPAlgo::default_algo (CPX_ALG_AUTOMATIC), and
    // CPX_PARAM_BARCROSSALG defaults to 0 (automatic crossover).  Force
    // the flag explicitly so opts.crossover==false truly disables it.
    CPXsetintparam(env,
                   CPX_PARAM_BARCROSSALG,
                   opts.crossover ? CPX_ALG_PRIMAL : CPX_ALG_NONE);
  }

  // SCRIND is intentionally NOT toggled here.  The screen-indicator is
  // owned by `apply_log_filename_to_env` (which turns it on only when
  // both `log_level > 0` AND a filename is set) and by the
  // `CplexEnvLp()` ctor (which forces it off as the silent default).
  // Toggling SCRIND on here based on `log_level` alone is the path
  // that lets CPLEX fall back to `./cplex.log` (and `clone1.log` /
  // `clone2.log` after `CPXcloneprob`) in the cwd of any caller that
  // bumps log_level without configuring a destination.
}

/// Enable CPLEX file logging with the provided basename + level.  When
/// level==0 or filename is empty, nothing is written — CPLEX stays silent
/// and no cplex.log is created.
void apply_log_filename_to_env(cpxenv* env,
                               const std::string& filename,
                               int level)
{
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    CPXsetlogfilename(env, log_path.c_str(), "a");
    CPXsetintparam(env, CPX_PARAM_SCRIND, CPX_ON);
  }
}

}  // namespace

// ============================================================
// CplexEnvLp — RAII for a CPLEX environment + problem pair.
// Its ctor is the single point where a fresh env+lp is opened,
// configured, and named; its dtor releases every byte CPLEX
// allocated for that pair.
// ============================================================

CplexEnvLp::CplexEnvLp()
{
  int status = 0;
  m_env_ = CPXopenCPLEX(&status);
  if (m_env_ == nullptr) {
    throw std::runtime_error(
        std::format("CPLEX: CPXopenCPLEX failed with status {}", status));
  }
  // Silent by default: no screen output, no log file.  Callers must
  // explicitly opt in via SolverOptions::log_level or set_log_filename().
  CPXsetintparam(m_env_, CPX_PARAM_SCRIND, CPX_OFF);
  // Suppress CPLEX's automatic per-clone log files (clone1.log,
  // clone2.log, ...) that `CPXcloneprob` would otherwise drop into the
  // process cwd whenever SCRIND happens to be on without a configured
  // logfile.  -1 = clone log disabled.
  CPXsetintparam(m_env_, CPX_PARAM_CLONELOG, -1);

  m_lp_ = CPXcreateprob(m_env_, &status, "gtopt");
  if (m_lp_ == nullptr) {
    CPXcloseCPLEX(&m_env_);
    throw std::runtime_error(
        std::format("CPLEX: CPXcreateprob failed with status {}", status));
  }
}

CplexEnvLp::CplexEnvLp(const CplexPrep& prep)
    : CplexEnvLp()
{
  if (prep.options.has_value()) {
    apply_options_to_env(m_env_, *prep.options);
  }
  apply_log_filename_to_env(m_env_, prep.log_filename, prep.log_level);
  if (!prep.prob_name.empty()) {
    CPXchgprobname(m_env_, m_lp_, prep.prob_name.c_str());
  }
}

CplexEnvLp::~CplexEnvLp()
{
  if (m_lp_ != nullptr) {
    CPXfreeprob(m_env_, &m_lp_);
  }
  if (m_env_ != nullptr) {
    CPXcloseCPLEX(&m_env_);
  }
}

CplexEnvLp::CplexEnvLp(CplexEnvLp&& other) noexcept
    : m_env_(std::exchange(other.m_env_, nullptr))
    , m_lp_(std::exchange(other.m_lp_, nullptr))
{
}

CplexEnvLp& CplexEnvLp::operator=(CplexEnvLp&& other) noexcept
{
  if (this != &other) {
    if (m_lp_ != nullptr) {
      CPXfreeprob(m_env_, &m_lp_);
    }
    if (m_env_ != nullptr) {
      CPXcloseCPLEX(&m_env_);
    }
    m_env_ = std::exchange(other.m_env_, nullptr);
    m_lp_ = std::exchange(other.m_lp_, nullptr);
  }
  return *this;
}

void CplexEnvLp::reset_lp(cpxlp* new_lp) noexcept
{
  if (m_lp_ != nullptr) {
    CPXfreeprob(m_env_, &m_lp_);
  }
  m_lp_ = new_lp;
}

// ============================================================
// CplexSolverBackend
// ============================================================

CplexSolverBackend::CplexSolverBackend() = default;
CplexSolverBackend::~CplexSolverBackend() = default;

void CplexSolverBackend::reset_env_lp()
{
  m_env_lp_ = CplexEnvLp {m_prep_};
}

std::string_view CplexSolverBackend::solver_name() const noexcept
{
  return "cplex";
}

std::string CplexSolverBackend::solver_version() const
{
  // CPX_VERSION format: VVRRMMFF (e.g. 22010100 → 22.1.1)
  constexpr int ver = CPX_VERSION;
  return std::format(
      "{}.{}.{}", ver / 1000000, (ver / 10000) % 100, (ver / 100) % 100);
}

double CplexSolverBackend::plugin_infinity() noexcept
{
  return CPX_INFBOUND;
}

double CplexSolverBackend::infinity() const noexcept
{
  return plugin_infinity();
}

bool CplexSolverBackend::supports_mip() const noexcept
{
  return true;
}

void CplexSolverBackend::set_prob_name(const std::string& name)
{
  m_prep_.prob_name = name;
  CPXchgprobname(m_env_lp_.env(), m_env_lp_.lp(), name.c_str());
}

std::string CplexSolverBackend::get_prob_name() const
{
  std::array<char, 256> buf {};
  int surplus = 0;
  if (CPXgetprobname(m_env_lp_.env(),
                     m_env_lp_.lp(),
                     buf.data(),
                     static_cast<int>(buf.size()),
                     &surplus)
      == 0)
  {
    return {buf.data()};
  }
  return {};
}

void CplexSolverBackend::load_problem(int ncols,
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
  invalidate_problem_data();
  m_solve_status_ = 0;

  // Cycle the entire CPLEX env+lp pair so every byte of per-LP state that
  // CPLEX allocated for the previous problem is released.  The new pair
  // is re-prepared (options, log filename, prob name) via m_prep_ inside
  // CplexEnvLp's constructor — one centralized place for env/lp setup.
  reset_env_lp();

  if (ncols == 0 && nrows == 0) {
    return;
  }

  int status = 0;

  // Convert row bounds to CPLEX sense/rhs/range
  std::vector<char> sense(static_cast<size_t>(nrows));
  std::vector<double> rhs(static_cast<size_t>(nrows));
  std::vector<double> range(static_cast<size_t>(nrows));
  const auto cpx_inf = CPX_INFBOUND;

  for (int i = 0; i < nrows; ++i) {
    const auto idx = static_cast<size_t>(i);
    bounds_to_cplex(
        rowlb[i],  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        rowub[i],  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        cpx_inf,
        sense[idx],
        rhs[idx],
        range[idx]);
  }

  // CPLEX requires sorted row indices within each column (CSC format).
  // `LinearProblem::flatten()` already produces sorted output: its
  // outer loop iterates rows in ascending `RowIndex` order, and the
  // per-column write cursor `colpos[c]` appends each entry as the
  // outer loop visits row r — so each column's `matind` slice is
  // automatically ascending in row index.
  //
  // Fast path: trust the caller and pass the buffers through to
  // `CPXcopylp` directly with NO copy, NO sort.  Saves on every
  // reconstruct under `LowMemoryMode::compress`:
  //   * 2 vector copies of the full matind/matval buffers
  //     (32k × (4+8) bytes ≈ 380 KB on juan/iplp).
  //   * Per-column allocation of `perm` / `temp_ind` / `temp_val`
  //     (3 small vectors × ~half of all cols ≈ 38k allocs per
  //     call on juan/iplp), and the sort within each column.
  //
  // Defensive path: a `GTOPT_CPLEX_VERIFY_MATIND_SORTED=1` env var
  // re-enables a debug-only verification pass that walks every
  // column and asserts ascending row indices.  Use this on a new
  // backend or after a flatten refactor to confirm the invariant
  // before relying on the fast path.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto nnz =
      (ncols > 0) ? static_cast<size_t>(matbeg[ncols]) : size_t {0};

  if (const auto* verify = std::getenv(
          "GTOPT_CPLEX_VERIFY_MATIND_SORTED");  // NOLINT(concurrency-mt-unsafe)
      verify != nullptr && *verify == '1' && matind != nullptr)
  {
    for (int col = 0; col < ncols; ++col) {
      const auto begin = static_cast<size_t>(matbeg[col]);
      const auto end =
          (col + 1 < ncols) ? static_cast<size_t>(matbeg[col + 1]) : nnz;
      for (auto k = begin + 1; k < end; ++k) {
        if (matind[k] < matind[k - 1]) {
          throw std::runtime_error(
              std::format("CPLEX: matind not sorted within column {} "
                          "(matind[{}]={} < matind[{}]={})",
                          col,
                          k,
                          matind[k],
                          k - 1,
                          matind[k - 1]));
        }
      }
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  // CPLEX requires all pointers to be non-null, even for zero-element
  // matrices.  Provide safe defaults when the caller passes nullptr —
  // but only allocate the fallback buffer for the pointer that is
  // actually missing, since in production every `load_problem` caller
  // hands over real arrays for all six and unconditional allocations
  // were pure waste (≈6 × ncols × 8 B per call).
  const auto ncols_sz = static_cast<size_t>(ncols);

  std::vector<int> buf_matbeg;
  std::array<int, 1> buf_matind {0};
  std::array<double, 1> buf_matval {0.0};
  std::vector<double> buf_obj;
  std::vector<double> buf_collb;
  std::vector<double> buf_colub;

  const int* safe_matbeg = matbeg;
  if (safe_matbeg == nullptr) {
    buf_matbeg.assign(ncols_sz + 1, 0);
    safe_matbeg = buf_matbeg.data();
  }
  const int* safe_matind =
      (matind != nullptr && nnz > 0) ? matind : buf_matind.data();
  const double* safe_matval =
      (matval != nullptr && nnz > 0) ? matval : buf_matval.data();
  const double* safe_obj = obj;
  if (safe_obj == nullptr) {
    buf_obj.assign(ncols_sz, 0.0);
    safe_obj = buf_obj.data();
  }
  const double* safe_collb = collb;
  if (safe_collb == nullptr) {
    buf_collb.assign(ncols_sz, 0.0);
    safe_collb = buf_collb.data();
  }
  const double* safe_colub = colub;
  if (safe_colub == nullptr) {
    buf_colub.assign(ncols_sz, 0.0);
    safe_colub = buf_colub.data();
  }

  // Compute matcnt from matbeg differences (CPLEX 22.1 requires it).
  std::vector<int> matcnt(ncols_sz, 0);
  for (int c = 0; c < ncols; ++c) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    matcnt[static_cast<size_t>(c)] = safe_matbeg[c + 1] - safe_matbeg[c];
  }

  // CPLEX expects column-sparse format via CPXcopylp
  status = CPXcopylp(m_env_lp_.env(),
                     m_env_lp_.lp(),
                     ncols,
                     nrows,
                     CPX_MIN,  // minimization
                     safe_obj,
                     rhs.data(),
                     sense.data(),
                     safe_matbeg,
                     matcnt.data(),
                     safe_matind,
                     safe_matval,
                     safe_collb,
                     safe_colub,
                     range.data());

  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXcopylp failed with status {}", status));
  }
}

int CplexSolverBackend::get_num_cols() const
{
  return CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
}

int CplexSolverBackend::get_num_rows() const
{
  return CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp());
}

void CplexSolverBackend::add_col(double lb, double ub, double obj)
{
  invalidate_problem_data();
  const int matbeg = 0;
  CPXaddcols(m_env_lp_.env(),
             m_env_lp_.lp(),
             1,
             0,
             &obj,
             &matbeg,
             nullptr,
             nullptr,
             &lb,
             &ub,
             nullptr);
}

void CplexSolverBackend::add_cols(int num_cols,
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

  // CPXaddcols takes the CSC format directly: ccnt, nzcnt, obj, cmatbeg,
  // cmatind, cmatval, lb, ub.  Single call replaces what would be N
  // per-column CPXaddcols invocations — each of those reallocates the
  // internal column metadata array, so the bulk path is materially
  // faster on cold-start LPs with thousands of columns.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int nzcnt = colbeg[num_cols];
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const int status = CPXaddcols(m_env_lp_.env(),
                                m_env_lp_.lp(),
                                num_cols,
                                nzcnt,
                                colobj,
                                colbeg,
                                colind,
                                colval,
                                collb,
                                colub,
                                nullptr);
  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXaddcols failed with status {}", status));
  }
}

void CplexSolverBackend::set_col_lower(int index, double value)
{
  invalidate_problem_data();
  const char bound_type = 'L';
  CPXchgbds(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &bound_type, &value);
}

void CplexSolverBackend::set_col_upper(int index, double value)
{
  invalidate_problem_data();
  const char bound_type = 'U';
  CPXchgbds(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &bound_type, &value);
}

void CplexSolverBackend::set_obj_coeff(int index, double value)
{
  invalidate_problem_data();
  CPXchgobj(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &value);
}

void CplexSolverBackend::set_obj_coeffs(const double* values, int num_cols)
{
  // CPXchgobj takes parallel `indices` and `values` arrays; there is no
  // "set all" form.  Build {0, 1, …, num_cols-1} once and dispatch a
  // single bulk call.  Use `std::iota` for clarity; the allocation is
  // amortised across `num_cols` per-element bookkeeping events that the
  // loop variant would have triggered.
  invalidate_problem_data();
  if (num_cols <= 0) {
    return;
  }
  std::vector<int> indices(static_cast<size_t>(num_cols));
  std::iota(indices.begin(), indices.end(), 0);
  CPXchgobj(m_env_lp_.env(), m_env_lp_.lp(), num_cols, indices.data(), values);
}

void CplexSolverBackend::add_row(int num_elements,
                                 const int* columns,
                                 const double* elements,
                                 double rowlb,
                                 double rowub)
{
  invalidate_problem_data();

  char sense {};
  double rhs {};
  double range {};
  bounds_to_cplex(rowlb, rowub, CPX_INFBOUND, sense, rhs, range);

  const int rmatbeg = 0;
  int status = CPXaddrows(m_env_lp_.env(),
                          m_env_lp_.lp(),
                          0,
                          1,
                          num_elements,
                          &rhs,
                          &sense,
                          &rmatbeg,
                          columns,
                          elements,
                          nullptr,
                          nullptr);
  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXaddrows failed with status {}", status));
  }

  // Set range value for ranged rows
  if (sense == 'R' && std::abs(range) > 1e-20) {
    const int row_idx = CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp()) - 1;
    CPXchgrngval(m_env_lp_.env(), m_env_lp_.lp(), 1, &row_idx, &range);
  }
}

void CplexSolverBackend::add_rows(int num_rows,
                                  const int* rowbeg,
                                  const int* rowind,
                                  const double* rowval,
                                  const double* rowlb,
                                  const double* rowub)
{
  invalidate_problem_data();

  // Convert lb/ub bounds to CPLEX sense/rhs/range vectors
  std::vector<char> senses(static_cast<size_t>(num_rows));
  std::vector<double> rhs_vec(static_cast<size_t>(num_rows));
  std::vector<double> range_vec(static_cast<size_t>(num_rows));

  for (int r = 0; r < num_rows; ++r) {
    bounds_to_cplex(
        rowlb[r], rowub[r], CPX_INFBOUND, senses[r], rhs_vec[r], range_vec[r]);
  }

  int status = CPXaddrows(m_env_lp_.env(),
                          m_env_lp_.lp(),
                          0,
                          num_rows,
                          rowbeg[num_rows],
                          rhs_vec.data(),
                          senses.data(),
                          rowbeg,
                          rowind,
                          rowval,
                          nullptr,
                          nullptr);
  if (status != 0) {
    throw std::runtime_error(
        std::format("CPLEX: CPXaddrows failed with status {}", status));
  }

  // Set range values for ranged rows
  const int first_new_row =
      CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp()) - num_rows;
  for (int r = 0; r < num_rows; ++r) {
    if (senses[r] == 'R' && std::abs(range_vec[r]) > 1e-20) {
      const int row_idx = first_new_row + r;
      CPXchgrngval(m_env_lp_.env(), m_env_lp_.lp(), 1, &row_idx, &range_vec[r]);
    }
  }
}

void CplexSolverBackend::set_row_lower(int index, double value)
{
  invalidate_problem_data();

  // Get current row upper bound to recompute sense/rhs/range
  char old_sense {};
  double old_rhs {};
  double old_range {};
  CPXgetrowinfeas(m_env_lp_.env(),
                  m_env_lp_.lp(),
                  nullptr,
                  nullptr,
                  0,
                  0);  // ensure internal state
  CPXgetsense(m_env_lp_.env(), m_env_lp_.lp(), &old_sense, index, index);
  CPXgetrhs(m_env_lp_.env(), m_env_lp_.lp(), &old_rhs, index, index);
  CPXgetrngval(m_env_lp_.env(), m_env_lp_.lp(), &old_range, index, index);

  const double old_ub =
      cplex_row_ub(old_sense, old_rhs, old_range, CPX_INFBOUND);

  char new_sense {};
  double new_rhs {};
  double new_range {};
  bounds_to_cplex(value, old_ub, CPX_INFBOUND, new_sense, new_rhs, new_range);

  CPXchgsense(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_sense);
  CPXchgrhs(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_rhs);
  CPXchgrngval(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_range);
}

void CplexSolverBackend::set_row_upper(int index, double value)
{
  invalidate_problem_data();

  char old_sense {};
  double old_rhs {};
  double old_range {};
  CPXgetsense(m_env_lp_.env(), m_env_lp_.lp(), &old_sense, index, index);
  CPXgetrhs(m_env_lp_.env(), m_env_lp_.lp(), &old_rhs, index, index);
  CPXgetrngval(m_env_lp_.env(), m_env_lp_.lp(), &old_range, index, index);

  const double old_lb =
      cplex_row_lb(old_sense, old_rhs, old_range, CPX_INFBOUND);

  char new_sense {};
  double new_rhs {};
  double new_range {};
  bounds_to_cplex(old_lb, value, CPX_INFBOUND, new_sense, new_rhs, new_range);

  CPXchgsense(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_sense);
  CPXchgrhs(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_rhs);
  CPXchgrngval(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_range);
}

void CplexSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  invalidate_problem_data();

  char new_sense {};
  double new_rhs {};
  double new_range {};
  bounds_to_cplex(lb, ub, CPX_INFBOUND, new_sense, new_rhs, new_range);

  CPXchgsense(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_sense);
  CPXchgrhs(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_rhs);
  CPXchgrngval(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &new_range);
}

void CplexSolverBackend::delete_rows(int num, const int* indices)
{
  invalidate_problem_data();

  // CPXdelrows expects a sorted range [begin, end].
  // We need to delete individual indices — use CPXdelsetrows with a delstat
  // array.
  const int nrows = CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp());
  std::vector<int> delstat(static_cast<size_t>(nrows), 0);
  for (int i = 0; i < num; ++i) {
    const auto idx = static_cast<size_t>(
        indices[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (idx < delstat.size()) {
      delstat[idx] = 1;
    }
  }
  CPXdelsetrows(m_env_lp_.env(), m_env_lp_.lp(), delstat.data());
}

double CplexSolverBackend::get_coeff(int row, int col) const
{
  double value = 0.0;
  CPXgetcoef(m_env_lp_.env(), m_env_lp_.lp(), row, col, &value);
  return value;
}

void CplexSolverBackend::set_coeff(int row, int col, double value)
{
  invalidate_problem_data();
  CPXchgcoef(m_env_lp_.env(), m_env_lp_.lp(), row, col, value);
}

bool CplexSolverBackend::supports_set_coeff() const noexcept
{
  return true;
}

void CplexSolverBackend::set_continuous(int index)
{
  // If problem is MIP, change column type to continuous
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    const char ctype = CPX_CONTINUOUS;
    CPXchgctype(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &ctype);
  }
}

void CplexSolverBackend::set_integer(int index)
{
  // Promote to MIP if needed
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    CPXchgprobtype(m_env_lp_.env(), m_env_lp_.lp(), CPXPROB_MILP);
  }
  const char ctype = CPX_INTEGER;
  CPXchgctype(m_env_lp_.env(), m_env_lp_.lp(), 1, &index, &ctype);
}

bool CplexSolverBackend::is_continuous(int index) const
{
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    return true;  // All continuous in LP
  }
  char ctype {};
  CPXgetctype(m_env_lp_.env(), m_env_lp_.lp(), &ctype, index, index);
  return ctype == CPX_CONTINUOUS;
}

bool CplexSolverBackend::is_integer(int index) const
{
  return !is_continuous(index);
}

void CplexSolverBackend::invalidate_problem_data() const noexcept
{
  m_collb_cached_ = false;
  m_colub_cached_ = false;
  m_obj_cached_ = false;
  m_rowbounds_cached_ = false;
}

void CplexSolverBackend::fill_collb_if_needed() const
{
  if (m_collb_cached_) {
    return;
  }
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  m_collb_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    CPXgetlb(m_env_lp_.env(), m_env_lp_.lp(), m_collb_.data(), 0, ncols - 1);
  }
  m_collb_cached_ = true;
}

void CplexSolverBackend::fill_colub_if_needed() const
{
  if (m_colub_cached_) {
    return;
  }
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  m_colub_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    CPXgetub(m_env_lp_.env(), m_env_lp_.lp(), m_colub_.data(), 0, ncols - 1);
  }
  m_colub_cached_ = true;
}

void CplexSolverBackend::fill_obj_if_needed() const
{
  if (m_obj_cached_) {
    return;
  }
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  m_obj_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    CPXgetobj(m_env_lp_.env(), m_env_lp_.lp(), m_obj_.data(), 0, ncols - 1);
  }
  m_obj_cached_ = true;
}

void CplexSolverBackend::fill_row_bounds_if_needed() const
{
  if (m_rowbounds_cached_) {
    return;
  }
  const int nrows = CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp());
  m_rowlb_.resize(static_cast<size_t>(nrows));
  m_rowub_.resize(static_cast<size_t>(nrows));
  if (nrows > 0) {
    std::vector<char> sense(static_cast<size_t>(nrows));
    std::vector<double> rhs(static_cast<size_t>(nrows));
    std::vector<double> range(static_cast<size_t>(nrows));
    CPXgetsense(m_env_lp_.env(), m_env_lp_.lp(), sense.data(), 0, nrows - 1);
    CPXgetrhs(m_env_lp_.env(), m_env_lp_.lp(), rhs.data(), 0, nrows - 1);
    CPXgetrngval(m_env_lp_.env(), m_env_lp_.lp(), range.data(), 0, nrows - 1);
    for (int i = 0; i < nrows; ++i) {
      const auto idx = static_cast<size_t>(i);
      m_rowlb_[idx] =
          cplex_row_lb(sense[idx], rhs[idx], range[idx], CPX_INFBOUND);
      m_rowub_[idx] =
          cplex_row_ub(sense[idx], rhs[idx], range[idx], CPX_INFBOUND);
    }
  }
  m_rowbounds_cached_ = true;
}

const double* CplexSolverBackend::col_lower() const
{
  fill_collb_if_needed();
  return m_collb_.data();
}

const double* CplexSolverBackend::col_upper() const
{
  fill_colub_if_needed();
  return m_colub_.data();
}

const double* CplexSolverBackend::obj_coefficients() const
{
  fill_obj_if_needed();
  return m_obj_.data();
}

const double* CplexSolverBackend::row_lower() const
{
  fill_row_bounds_if_needed();
  return m_rowlb_.data();
}

const double* CplexSolverBackend::row_upper() const
{
  fill_row_bounds_if_needed();
  return m_rowub_.data();
}

// Solution accessors: each refills *only its own* buffer via the
// matching CPXget* call.  No cross-accessor work, no validity flag,
// no caching semantics — the buffer is plain scratch storage for the
// C-API write target.  The single caching layer is in
// `LinearInterface::populate_solution_cache_post_solve`.
const double* CplexSolverBackend::col_solution() const
{
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  m_col_solution_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    CPXgetx(
        m_env_lp_.env(), m_env_lp_.lp(), m_col_solution_.data(), 0, ncols - 1);
  }
  return m_col_solution_.data();
}

const double* CplexSolverBackend::reduced_cost() const
{
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  m_reduced_cost_.resize(static_cast<size_t>(ncols));
  if (ncols > 0) {
    CPXgetdj(
        m_env_lp_.env(), m_env_lp_.lp(), m_reduced_cost_.data(), 0, ncols - 1);
  }
  return m_reduced_cost_.data();
}

const double* CplexSolverBackend::row_price() const
{
  const int nrows = CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp());
  m_row_price_.resize(static_cast<size_t>(nrows));
  if (nrows > 0) {
    CPXgetpi(
        m_env_lp_.env(), m_env_lp_.lp(), m_row_price_.data(), 0, nrows - 1);
  }
  return m_row_price_.data();
}

// Span-out fills: write directly into the caller's buffer via the
// CPLEX C-API, never touching the per-instance scratch members.
// Used by `LinearInterface::populate_solution_cache_post_solve` in
// compress / rebuild mode so the LI's `m_cached_*` is the sole
// destination — no plugin-side allocation, no second copy.

void CplexSolverBackend::fill_col_sol(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  CPXgetx(m_env_lp_.env(),
          m_env_lp_.lp(),
          out.data(),
          0,
          static_cast<int>(out.size()) - 1);
}

void CplexSolverBackend::fill_col_cost(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  CPXgetdj(m_env_lp_.env(),
           m_env_lp_.lp(),
           out.data(),
           0,
           static_cast<int>(out.size()) - 1);
}

void CplexSolverBackend::fill_row_dual(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  CPXgetpi(m_env_lp_.env(),
           m_env_lp_.lp(),
           out.data(),
           0,
           static_cast<int>(out.size()) - 1);
}

double CplexSolverBackend::obj_value() const
{
  double val = 0.0;
  CPXgetobjval(m_env_lp_.env(), m_env_lp_.lp(), &val);
  return val;
}

void CplexSolverBackend::set_col_solution(const double* sol)
{
  if (sol == nullptr) {
    return;
  }
  // Provide the solution to CPLEX as a warm-start hint.  The
  // backend no longer maintains an internal solution cache; the next
  // `col_solution()` call after a successful resolve will re-fetch
  // via `CPXgetx`.  This was previously stuffed into m_col_solution_
  // so a "read-without-solve" path could observe the hint, but no
  // production caller actually reads the primal between
  // set_col_solution and resolve — the test path is always
  // set → solve → read.
  CPXcopystart(m_env_lp_.env(),
               m_env_lp_.lp(),
               nullptr,  // cstat (basis statuses for columns)
               nullptr,  // rstat (basis statuses for rows)
               sol,  // primal values
               nullptr,  // dual values
               nullptr,  // slack values
               nullptr);  // dj values
}

void CplexSolverBackend::set_row_price(const double* price)
{
  if (price == nullptr) {
    return;
  }
  CPXcopystart(m_env_lp_.env(),
               m_env_lp_.lp(),
               nullptr,  // cstat
               nullptr,  // rstat
               nullptr,  // primal
               price,  // dual
               nullptr,  // slack
               nullptr);  // dj
}

void CplexSolverBackend::initial_solve()
{
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    m_solve_status_ = CPXmipopt(m_env_lp_.env(), m_env_lp_.lp());
  } else {
    m_solve_status_ = CPXlpopt(m_env_lp_.env(), m_env_lp_.lp());
  }
}

void CplexSolverBackend::resolve()
{
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    m_solve_status_ = CPXmipopt(m_env_lp_.env(), m_env_lp_.lp());
  } else {
    // CPXlpopt respects CPX_PARAM_LPMETHOD set by apply_options().
    m_solve_status_ = CPXlpopt(m_env_lp_.env(), m_env_lp_.lp());
  }
}

void CplexSolverBackend::engage_robust_solve()
{
  auto* env = m_env_lp_.env();
  if (env == nullptr) {
    return;
  }

  // Capture baseline parameters on the *first* engage so that disengage
  // always restores the very first state we observed.  Subsequent
  // engages (escalations) bump tolerances another × 10 without
  // overwriting the baseline snapshot.
  if (!m_saved_robust_state_.has_value()) {
    RobustState saved {};
    CPXgetdblparam(env, CPX_PARAM_EPOPT, &saved.epopt);
    CPXgetdblparam(env, CPX_PARAM_EPRHS, &saved.eprhs);
    CPXgetdblparam(env, CPX_PARAM_BAREPCOMP, &saved.barepcomp);
    CPXgetintparam(env, CPX_PARAM_NUMERICALEMPHASIS, &saved.numerical_emphasis);
    CPXgetintparam(env, CPX_PARAM_PERIND, &saved.perind);
    CPXgetintparam(env, CPX_PARAM_REPEATPRESOLVE, &saved.repeat_presolve);
    saved.engage_count = 0;
    m_saved_robust_state_ = saved;
  }
  ++m_saved_robust_state_->engage_count;

  // Loosen tolerances by an additional × 10 multiplier per engage.  The
  // base values must come from the live env (after any previous engage)
  // so the chain naturally compounds: ×10 → ×100 → ×1000.
  double cur_epopt = 0.0;
  double cur_eprhs = 0.0;
  double cur_barepcomp = 0.0;
  CPXgetdblparam(env, CPX_PARAM_EPOPT, &cur_epopt);
  CPXgetdblparam(env, CPX_PARAM_EPRHS, &cur_eprhs);
  CPXgetdblparam(env, CPX_PARAM_BAREPCOMP, &cur_barepcomp);

  // CPLEX caps EPOPT/EPRHS at 1e-1 — clamp to the documented upper bound.
  constexpr double k_max_simplex_tol = 1e-1;
  constexpr double k_max_barrier_tol = 1e-1;
  CPXsetdblparam(
      env, CPX_PARAM_EPOPT, std::min(cur_epopt * 10.0, k_max_simplex_tol));
  CPXsetdblparam(
      env, CPX_PARAM_EPRHS, std::min(cur_eprhs * 10.0, k_max_simplex_tol));
  CPXsetdblparam(env,
                 CPX_PARAM_BAREPCOMP,
                 std::min(cur_barepcomp * 10.0, k_max_barrier_tol));

  // Numerical emphasis: trade speed for stability.  Perturbation index
  // forces simplex to perturb bounds preemptively (helps degenerate LPs).
  // RepeatPresolve=3 = aggressively re-presolve at the root.
  CPXsetintparam(env, CPX_PARAM_NUMERICALEMPHASIS, CPX_ON);
  CPXsetintparam(env, CPX_PARAM_PERIND, CPX_ON);
  CPXsetintparam(env, CPX_PARAM_REPEATPRESOLVE, 3);
}

void CplexSolverBackend::disengage_robust_solve() noexcept
{
  if (!m_saved_robust_state_.has_value()) {
    return;
  }
  auto* env = m_env_lp_.env();
  if (env == nullptr) {
    m_saved_robust_state_.reset();
    return;
  }

  const auto& s = *m_saved_robust_state_;
  CPXsetdblparam(env, CPX_PARAM_EPOPT, s.epopt);
  CPXsetdblparam(env, CPX_PARAM_EPRHS, s.eprhs);
  CPXsetdblparam(env, CPX_PARAM_BAREPCOMP, s.barepcomp);
  CPXsetintparam(env, CPX_PARAM_NUMERICALEMPHASIS, s.numerical_emphasis);
  CPXsetintparam(env, CPX_PARAM_PERIND, s.perind);
  CPXsetintparam(env, CPX_PARAM_REPEATPRESOLVE, s.repeat_presolve);
  m_saved_robust_state_.reset();
}

bool CplexSolverBackend::is_proven_optimal() const
{
  const int stat = CPXgetstat(m_env_lp_.env(), m_env_lp_.lp());
  return stat == CPX_STAT_OPTIMAL || stat == CPXMIP_OPTIMAL
      || stat == CPXMIP_OPTIMAL_TOL;
}

bool CplexSolverBackend::is_abandoned() const
{
  const int stat = CPXgetstat(m_env_lp_.env(), m_env_lp_.lp());
  return stat == CPX_STAT_ABORT_USER || stat == CPX_STAT_ABORT_IT_LIM
      || stat == CPX_STAT_ABORT_TIME_LIM || stat == CPX_STAT_ABORT_OBJ_LIM
      || stat == CPX_STAT_NUM_BEST;
}

bool CplexSolverBackend::is_proven_primal_infeasible() const
{
  const int stat = CPXgetstat(m_env_lp_.env(), m_env_lp_.lp());
  return stat == CPX_STAT_INFEASIBLE || stat == CPXMIP_INFEASIBLE;
}

bool CplexSolverBackend::is_proven_dual_infeasible() const
{
  const int stat = CPXgetstat(m_env_lp_.env(), m_env_lp_.lp());
  return stat == CPX_STAT_UNBOUNDED || stat == CPXMIP_UNBOUNDED;
}

LPAlgo CplexSolverBackend::get_algorithm() const
{
  return m_algorithm_;
}

int CplexSolverBackend::get_threads() const
{
  return m_threads_;
}

bool CplexSolverBackend::get_presolve() const
{
  return m_presolve_;
}

int CplexSolverBackend::get_log_level() const
{
  return m_log_level_;
}

SolverOptions CplexSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::barrier,
      .threads = 4,
      .presolve = true,
      .scaling = SolverScaling::automatic,
      .max_fallbacks = 2,
  };
}

void CplexSolverBackend::apply_options(const SolverOptions& opts)
{
  m_prep_.options = opts;
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;
  apply_options_to_env(m_env_lp_.env(), opts);
}

std::optional<double> CplexSolverBackend::get_kappa() const
{
  // CPX_KAPPA requires a valid basis.  After a barrier solve without
  // crossover CPLEX has no basis and CPXgetdblquality returns a non-zero
  // status; we propagate that as nullopt.  We do NOT try to sniff the
  // solution method here — the CPXgetdblquality return code is the
  // authoritative signal.
  double kappa = 0.0;
  if (CPXgetdblquality(m_env_lp_.env(), m_env_lp_.lp(), &kappa, CPX_KAPPA) != 0)
  {
    return std::nullopt;
  }
  return kappa;
}

void CplexSolverBackend::open_log(FILE* /*file*/, int level)
{
  CPXsetintparam(
      m_env_lp_.env(), CPX_PARAM_SCRIND, level > 0 ? CPX_ON : CPX_OFF);
}

void CplexSolverBackend::close_log()
{
  CPXsetintparam(m_env_lp_.env(), CPX_PARAM_SCRIND, CPX_OFF);
}

void CplexSolverBackend::set_log_filename(const std::string& filename,
                                          int level)
{
  // Only enable file logging when the caller actually wants it.  Otherwise
  // leave CPLEX silent — critical for avoiding a spurious cplex.log in the
  // working directory.
  if (level > 0 && !filename.empty()) {
    m_prep_.log_filename = filename;
    m_prep_.log_level = level;
    apply_log_filename_to_env(m_env_lp_.env(), filename, level);
  }
}

void CplexSolverBackend::clear_log_filename()
{
  m_prep_.log_filename.clear();
  m_prep_.log_level = 0;
  CPXsetlogfilename(m_env_lp_.env(), nullptr, nullptr);
  CPXsetintparam(m_env_lp_.env(), CPX_PARAM_SCRIND, CPX_OFF);
}

void CplexSolverBackend::push_names(const std::vector<std::string>& col_names,
                                    const std::vector<std::string>& row_names)
{
  // Set column names — copy to mutable buffer because older CPLEX APIs
  // declare the name parameter as char** (non-const).
  for (int i = 0; std::cmp_less(i, col_names.size()); ++i) {
    if (!col_names[static_cast<size_t>(i)].empty()) {
      std::string name_buf = col_names[static_cast<size_t>(i)];
      auto* name_ptr = name_buf.data();
      CPXchgcolname(m_env_lp_.env(), m_env_lp_.lp(), 1, &i, &name_ptr);
    }
  }

  // Set row names
  for (int i = 0; std::cmp_less(i, row_names.size()); ++i) {
    if (!row_names[static_cast<size_t>(i)].empty()) {
      std::string name_buf = row_names[static_cast<size_t>(i)];
      auto* name_ptr = name_buf.data();
      CPXchgrowname(m_env_lp_.env(), m_env_lp_.lp(), 1, &i, &name_ptr);
    }
  }
}

void CplexSolverBackend::write_lp(const char* filename)
{
  const auto file = std::format("{}.lp", filename);
  CPXwriteprob(m_env_lp_.env(), m_env_lp_.lp(), file.c_str(), "LP");
}

std::unique_ptr<SolverBackend> CplexSolverBackend::clone() const
{
  auto cloned = std::make_unique<CplexSolverBackend>();

  // The clone owns its own env+lp pair — no sharing with the source.
  //
  //   1. Propagate the preparation state so the new env mirrors the
  //      source's options, log settings, and prob name.
  //   2. Rebuild env+lp through CplexEnvLp's ctor: a fresh CPXopenCPLEX
  //      call yields an independent environment, and a fresh
  //      CPXcreateprob gives it an empty problem.
  //   3. Deep-copy the source problem into the clone's own environment
  //      via CPXcloneprob, replacing the empty lp.
  //
  // When the returned clone is destroyed, CplexEnvLp's destructor calls
  // CPXfreeprob on its own lp and CPXcloseCPLEX on its own env — no
  // aliasing with the source, so destruction is always clean.
  cloned->m_prep_ = m_prep_;
  cloned->m_algorithm_ = m_algorithm_;
  cloned->m_threads_ = m_threads_;
  cloned->m_presolve_ = m_presolve_;
  cloned->m_log_level_ = m_log_level_;
  cloned->reset_env_lp();

  int status = 0;
  cpxlp* const cloned_lp =
      CPXcloneprob(cloned->m_env_lp_.env(), m_env_lp_.lp(), &status);
  if (cloned_lp == nullptr) {
    throw std::runtime_error(
        std::format("CPLEX: CPXcloneprob failed with status {}", status));
  }
  cloned->m_env_lp_.reset_lp(cloned_lp);

  return cloned;
}

}  // namespace gtopt
