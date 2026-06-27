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
#include <cstdio>
#include <filesystem>
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

/// Configure a warm (advanced-basis) simplex re-solve on `env`.  Shared by
/// the two warm-start paths so they stay in lock-step:
///   1. the `SolverOptions::advanced_basis` option (SDDP aperture
///      warm-start — re-optimize off the previous aperture's basis after a
///      column-bound change), and
///   2. the fixed-MILP dual-extraction pass (`fix_mip_and_resolve_duals` —
///      re-optimize off the incumbent node's basis after fixing integers).
/// Both re-optimize off a basis already resident on the problem object,
/// which only the simplex methods can do — barrier cannot warm-start.
///
/// In-code default: `ADVIND=1` + primal/dual simplex (per `opts.algorithm`,
/// default primal — empirically best on the GTEP subproblems).  Users can
/// override the whole warm pass with a `cplex_warmstart.prm` sibling next
/// to the case's main `.prm` — the SINGLE shared warm-start param file
/// (carries `CPXPARAM_LPMethod 1` primal + `CPXPARAM_Advance 1`), shipped by
/// plp2gtopt / plexos2gtopt and loaded for BOTH warm paths.  Call this AFTER
/// the main `.prm` so it wins over a pinned `LPMethod` (the bundled
/// `cplex.prm` forces barrier).
void apply_cplex_warmstart(cpxenv* env, const SolverOptions& opts)
{
  CPXsetintparam(env, CPX_PARAM_ADVIND, 1);
  const int warm_method =
      (opts.algorithm == LPAlgo::dual) ? CPX_ALG_DUAL : CPX_ALG_PRIMAL;
  CPXsetintparam(env, CPX_PARAM_LPMETHOD, warm_method);

  // Optional user override: the case's `cplex_warmstart.prm` sibling (next
  // to its main `cplex.prm`) — the single shared warm-start param file
  // (`LPMethod 1` primal + `Advance 1`), shipped by plp2gtopt / plexos2gtopt
  // and used by BOTH the fixed-MILP dual pass and the aperture warm-start.
  // Loaded last so it wins over the in-code defaults above.
  if (opts.param_file.has_value() && !opts.param_file->empty()) {
    const std::filesystem::path sibling =
        std::filesystem::path {*opts.param_file}.parent_path()
        / "cplex_warmstart.prm";
    std::error_code ec;
    if (std::filesystem::exists(sibling, ec) && !ec) {
      CPXreadcopyparam(env, sibling.string().c_str());
    }
  }
}

/// Apply a SolverOptions bundle onto a CPLEX env.  Pure env-level mutation
/// — never touches backend member fields.  Shared between CplexEnvLp's
/// ctor (fresh env preparation) and CplexSolverBackend::apply_options()
/// (live in-place tweaks).
void apply_options_to_env(cpxenv* env, const SolverOptions& opts)
{
  // Read the user-supplied .prm FIRST so it forms the BASE that the typed
  // gtopt SolverOptions below OVERRIDE — matching the field doc
  // ("param_file applied before the fields above, so the typed gtopt fields
  // keep priority on conflict").  This ordering is load-bearing:
  // ``CPXreadcopyparam`` RESETS every parameter to its CPLEX default before
  // applying the file, so any typed value set BEFORE it is silently wiped.
  // The old order put this read LAST, which is why
  // ``--set solver_options.presolve=false`` (and scaling, threads, …) had no
  // effect whenever a ``.prm`` was pinned.  Params the .prm does not mention
  // (e.g. the tuned LPMethod / Gomory cuts in the bundled cplex.prm) survive,
  // provided gtopt does not also set them — see the ``default_algo`` case
  // below, which deliberately leaves LPMethod to the .prm.
  if (opts.param_file.has_value() && !opts.param_file->empty()) {
    const int rc = CPXreadcopyparam(env, opts.param_file->c_str());
    if (rc != 0) {
      std::fprintf(stderr,
                   "[CPLEX] WARN: CPXreadcopyparam('%s') returned status %d — "
                   "fell back to gtopt-set defaults.\n",
                   opts.param_file->c_str(),
                   rc);
    } else {
      std::fprintf(stderr,
                   "[CPLEX] INFO: read parameter file '%s' (typed gtopt "
                   "options below override its values)\n",
                   opts.param_file->c_str());
    }
  }

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
  // Target relative MIP optimality gap (CPLEX `CPX_PARAM_EPGAP`).
  // Continuous LPs ignore the parameter so we can apply it
  // unconditionally — `*gap > 0` guards against a misconfigured zero
  // that would mean "no gap tolerance" and could starve CPLEX of an
  // exit condition on poorly-scaled MIPs.  Apply BEFORE the algorithm
  // switch so a barrier override does not silently drop it.
  if (const auto gap = opts.mip_gap; gap && *gap > 0) {
    CPXsetdblparam(env, CPX_PARAM_EPGAP, *gap);
  }
  // Target ABSOLUTE MIP optimality gap (CPLEX `CPX_PARAM_EPAGAP`).
  // Unlike the relative EPGAP, the absolute gap is invariant to a constant
  // objective offset (e.g. an FCF cost-to-go folded in via
  // `add_obj_constant`), so it stays meaningful when the objective carries
  // a large baseline.  Value is in the solver's (scale_objective-scaled)
  // objective units.  The bundled `cplex.prm` may also set
  // `CPXPARAM_MIP_Tolerances_AbsMIPGap`, which overrides this.
  if (const auto gap_abs = opts.mip_gap_abs; gap_abs && *gap_abs > 0.0) {
    CPXsetdblparam(env, CPX_PARAM_EPAGAP, *gap_abs);
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
        // No explicit algorithm preference.  A ``.prm`` read above may have
        // pinned LPMethod (e.g. the bundled cplex.prm's barrier) — leave that
        // in place.  But with NO ``.prm`` we must still set CPLEX's automatic
        // explicitly: this env is frequently REUSED / cloned across solves, so
        // an unconditional skip would let it inherit a residual LPMethod from a
        // prior solve (a dual/barrier leftover), which shifted juan's otherwise
        // deterministic ticks and is non-reproducible.
        if (!opts.param_file.has_value() || opts.param_file->empty()) {
          CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_AUTOMATIC);
        }
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

  // Advanced (warm) basis start — applied LAST (after the main .prm) via
  // the shared helper, so it wins over any LPMethod the .prm pins (the
  // bundled cplex.prm forces barrier, which cannot warm-start).  Reuses
  // the basis resident on this problem object after a column-bound change.
  if (opts.advanced_basis) {
    apply_cplex_warmstart(env, opts);
  } else if (opts.force_barrier_crossover) {
    // Cold-canonical anchor (no basis to warm-start): force barrier + primal
    // crossover AFTER the .prm so it wins over a pinned LPMethod (e.g. the
    // bundled cplex.prm forces dual simplex).  Produces a deterministic vertex
    // basis fast — the SDDP coordinated-seed scheme's first-aperture solve.
    CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_BARRIER);
    CPXsetintparam(env, CPX_PARAM_BARCROSSALG, CPX_ALG_PRIMAL);
  }
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

void CplexSolverBackend::set_col_bounds_bulk(int num,
                                             const int* indices,
                                             const char* lu,
                                             const double* values)
{
  // CPXchgbds accepts parallel `(indices, lu, bd)` arrays in one call.
  // The 'B' (both) case is expanded into one 'L' + one 'U' pair because
  // CPXchgbds doesn't accept 'B' directly — it interprets each row of
  // `lu` as a single side selector.  For chunks with no 'B' entries
  // (the typical aperture pending-bound replay path) the values arrays
  // are forwarded verbatim, costing one CPXchgbds dispatch instead of
  // N virtual calls.
  invalidate_problem_data();
  if (num <= 0) {
    return;
  }
  // Fast path: no 'B' (both-sides) entries — forward parallel arrays
  // unchanged.  Single allocation-free CPXchgbds dispatch.
  bool has_both = false;
  for (int i = 0; i < num; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (lu[i] == 'B') {
      has_both = true;
      break;
    }
  }
  if (!has_both) {
    CPXchgbds(m_env_lp_.env(), m_env_lp_.lp(), num, indices, lu, values);
    return;
  }
  // Expand 'B' into 'L'+'U' pairs.  Worst case the expanded vectors are
  // 2× the input; reserve up front to skip reallocations.
  std::vector<int> idx;
  std::vector<char> sides;
  std::vector<double> vals;
  idx.reserve(static_cast<size_t>(num) * 2U);
  sides.reserve(static_cast<size_t>(num) * 2U);
  vals.reserve(static_cast<size_t>(num) * 2U);
  for (int i = 0; i < num; ++i) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char c = lu[i];
    if (c == 'B') {
      idx.push_back(indices[i]);
      sides.push_back('L');
      vals.push_back(values[i]);
      idx.push_back(indices[i]);
      sides.push_back('U');
      vals.push_back(values[i]);
    } else {
      idx.push_back(indices[i]);
      sides.push_back(c);
      vals.push_back(values[i]);
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  CPXchgbds(m_env_lp_.env(),
            m_env_lp_.lp(),
            static_cast<int>(idx.size()),
            idx.data(),
            sides.data(),
            vals.data());
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

void CplexSolverBackend::add_sos2(std::span<const int> columns)
{
  // SOS2 declaration is a structural mutation — flush every per-call
  // cache the way ``invalidate_problem_data()`` would.  The native
  // ``CPXaddsos`` does not touch the LP matrix itself, but it does
  // change the problem type to MIP if it was pure LP, which the
  // bound/obj caches do depend on.
  if (columns.size() < 2) {
    return;
  }
  invalidate_problem_data();

  // CPLEX expects one (sostype, beg, ind, wt) record per call.  We
  // emit a single SOS2 here, so:
  //   numsos = 1, numsosnz = columns.size(), sosbeg = {0}
  //   sostype = "2"  (CPX_TYPE_SOS2)
  //   sosind  = columns (caller-owned, contiguous)
  //   soswt   = {1, 2, …, N}  geometric breakpoint order; gtopt
  //                            does not expose custom weights since
  //                            the LP-side segment widths already
  //                            encode the breakpoint distances.
  const int num_sos = 1;
  const int num_nnz = static_cast<int>(columns.size());
  const int sosbeg = 0;
  const char sostype = CPX_TYPE_SOS2;

  std::vector<double> wt;
  wt.reserve(columns.size());
  for (std::size_t i = 0; i < columns.size(); ++i) {
    wt.push_back(static_cast<double>(i + 1));
  }

  // Promote to MIP if needed — CPXaddsos auto-promotes on most CPLEX
  // versions but we mirror set_integer's defensive flip so the SOS2
  // contract is symmetric with the integer-column contract.
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    CPXchgprobtype(m_env_lp_.env(), m_env_lp_.lp(), CPXPROB_MILP);
  }

  const int rc = CPXaddsos(m_env_lp_.env(),
                           m_env_lp_.lp(),
                           num_sos,
                           num_nnz,
                           &sostype,
                           &sosbeg,
                           columns.data(),
                           wt.data(),
                           nullptr);
  if (rc != 0) {
    std::array<char, CPXMESSAGEBUFSIZE> err_buf {};
    CPXgeterrorstring(m_env_lp_.env(), rc, err_buf.data());
    throw std::runtime_error(
        std::format("CplexSolverBackend::add_sos2 CPXaddsos failed "
                    "(rc={}, n={}): {}",
                    rc,
                    num_nnz,
                    err_buf.data()));
  }
}

int CplexSolverBackend::relax_all_integers()
{
  // Single-call bulk relaxation: switch the problem type from
  // MILP/MIQP to LP/QP — CPLEX internally flips every integer
  // column to continuous as part of `CPXchgprobtype`, far cheaper
  // than per-column `CPXchgctype` round-trips.  Already-LP
  // problems are a no-op.
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    return 0;  // pure LP / QP — nothing to relax
  }

  // Count integer columns BEFORE the flip — `CPXgetnumint` is O(1)
  // (CPLEX maintains the count internally).  After
  // `CPXchgprobtype(... CPXPROB_LP)` every column is continuous.
  const int int_count = CPXgetnumint(m_env_lp_.env(), m_env_lp_.lp());
  const int bin_count = CPXgetnumbin(m_env_lp_.env(), m_env_lp_.lp());
  const int relaxed = int_count + bin_count;

  const int target_type =
      (cplex_type == CPXPROB_MIQP) ? CPXPROB_QP : CPXPROB_LP;
  CPXchgprobtype(m_env_lp_.env(), m_env_lp_.lp(), target_type);
  return relaxed;
}

int CplexSolverBackend::fix_mip_and_resolve_duals(const SolverOptions& opts)
{
  auto* env = m_env_lp_.env();
  auto* lp = m_env_lp_.lp();

  // 1. Pure LP / QP — nothing to fix; caller's duals are already valid.
  const int cplex_type = CPXgetprobtype(env, lp);
  if (cplex_type != CPXPROB_MILP && cplex_type != CPXPROB_MIQP) {
    return 0;
  }

  // 2. Require an incumbent.  `CPXPROB_FIXEDMILP` needs the MIP to have
  //    found (and stored) an integer-feasible solution; without one there
  //    is nothing to fix and the caller must keep the MIP result.
  const int stat = CPXgetstat(env, lp);
  const bool has_incumbent = stat == CPXMIP_OPTIMAL
      || stat == CPXMIP_OPTIMAL_TOL || stat == CPXMIP_OPTIMAL_INFEAS;
  if (!has_incumbent) {
    return -1;
  }

  // 3. Count the discrete columns BEFORE switching problem type
  //    (`CPXgetnumint` / `CPXgetnumbin` are O(1)).  After the type flip
  //    every column is continuous, so the counts must be read now.
  const int fixed = CPXgetnumint(env, lp) + CPXgetnumbin(env, lp);

  // 4. `CPXPROB_FIXEDMILP` fixes every discrete variable to its incumbent
  //    value AND installs the incumbent node's optimal simplex basis.  A
  //    warm simplex off that basis is a handful of pivots — no barrier, no
  //    crossover.  If the conversion fails, no state changed: bail out and
  //    let the caller keep the MIP result.
  if (CPXchgprobtype(env, lp, CPXPROB_FIXEDMILP) != 0) {
    return -1;
  }

  // 5. Apply the caller options for the fixed pass, but DO NOT re-read the
  //    bundled barrier `cplex.prm` — that file forces `LPMethod 4` (barrier)
  //    and, re-loaded here, would re-presolve the fixed LP from scratch
  //    (barrier root relaxation, crossover), turning the supposedly-warm
  //    fixed pass into a full cold re-solve as expensive as the MIP itself.
  SolverOptions fixed_opts = opts;
  fixed_opts.param_file.reset();

  // Save the current LP method (the cold MIP-root barrier setting) and
  // restore it afterwards so subsequent cold solves keep using barrier.
  int saved_lpmethod = CPX_ALG_AUTOMATIC;
  CPXgetintparam(env, CPX_PARAM_LPMETHOD, &saved_lpmethod);
  int saved_advance = 1;
  CPXgetintparam(env, CPX_PARAM_ADVIND, &saved_advance);

  apply_options(fixed_opts);

  // The four requirements that make `CPXPROB_FIXEDMILP` warm: (1) same live
  // object / no reconstruct between MIP solve and here [caller], (2)
  // ADVIND=1 to USE the incumbent's advanced basis, (3) primal simplex (a
  // few pivots off the incumbent basis), not barrier, (4) MIP solved to
  // optimality first [checked above].  (2)+(3) are configured by the
  // SHARED `apply_cplex_warmstart` helper — the SAME warm-start path the
  // `advanced_basis` option uses for SDDP apertures — applied AFTER
  // `apply_options` so it wins over any loaded `.prm`.  Pass the ORIGINAL
  // `opts` (with its `param_file`) so the helper can resolve the optional
  // `cplex_warmstart.prm` sibling relative to the case's main `.prm`.
  apply_cplex_warmstart(env, opts);

  // 6. Warm primal-simplex solve of the fixed LP off the incumbent basis.
  //    If it does not reach optimality, retry once with barrier on the
  //    still-fixed problem as a safety net.
  m_solve_status_ = CPXlpopt(env, lp);
  if (CPXgetstat(env, lp) != CPX_STAT_OPTIMAL) {
    CPXsetintparam(env, CPX_PARAM_LPMETHOD, CPX_ALG_BARRIER);
    m_solve_status_ = CPXlpopt(env, lp);
  }

  // 7. Restore the saved LP method + advance flag so subsequent cold solves
  //    keep barrier / their prior advanced-start behaviour.
  CPXsetintparam(env, CPX_PARAM_LPMETHOD, saved_lpmethod);
  CPXsetintparam(env, CPX_PARAM_ADVIND, saved_advance);

  // 8. Report the fixed count, or -1 if the fixed-LP solve still failed.
  if (CPXgetstat(env, lp) != CPX_STAT_OPTIMAL) {
    return -1;
  }
  return fixed;
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

void CplexSolverBackend::fill_col_lower(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  CPXgetlb(m_env_lp_.env(),
           m_env_lp_.lp(),
           out.data(),
           0,
           static_cast<int>(out.size()) - 1);
}

void CplexSolverBackend::fill_col_upper(std::span<double> out) const
{
  if (out.empty()) {
    return;
  }
  CPXgetub(m_env_lp_.env(),
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

bool CplexSolverBackend::set_mip_start(std::span<const double> col_values,
                                       MipStartEffort effort)
{
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  if (col_values.empty() || static_cast<int>(col_values.size()) != ncols) {
    return false;
  }

  // Map the gtopt effort enum onto CPLEX's CPX_MIPSTART_* effort level.
  int effortlevel = CPX_MIPSTART_CHECKFEAS;
  switch (effort) {
    case MipStartEffort::check_feasibility:
      effortlevel = CPX_MIPSTART_CHECKFEAS;
      break;
    case MipStartEffort::solve_fixed:
      effortlevel = CPX_MIPSTART_SOLVEFIXED;
      break;
    case MipStartEffort::solve_mip:
      effortlevel = CPX_MIPSTART_SOLVEMIP;
      break;
    case MipStartEffort::repair:
      effortlevel = CPX_MIPSTART_REPAIR;
      break;
    case MipStartEffort::no_check:
      effortlevel = CPX_MIPSTART_NOCHECK;
      break;
  }

  // One dense MIP start over all columns: indices 0..ncols-1, beg = {0}.
  std::vector<int> indices(static_cast<std::size_t>(ncols));
  std::iota(indices.begin(), indices.end(), 0);
  const int beg = 0;

  const int rc = CPXaddmipstarts(m_env_lp_.env(),
                                 m_env_lp_.lp(),
                                 1,  // mcnt: one start set
                                 ncols,  // nzcnt
                                 &beg,  // beg
                                 indices.data(),
                                 col_values.data(),
                                 &effortlevel,
                                 nullptr);  // mipstartname
  if (rc != 0) {
    std::array<char, CPXMESSAGEBUFSIZE> err_buf {};
    CPXgeterrorstring(m_env_lp_.env(), rc, err_buf.data());
    throw std::runtime_error(
        std::format("CplexSolverBackend::set_mip_start CPXaddmipstarts "
                    "failed (rc={}, ncols={}): {}",
                    rc,
                    ncols,
                    err_buf.data()));
  }
  return true;
}

int CplexSolverBackend::restore_integers(std::span<const int> integer_cols)
{
  // Inverse of relax_all_integers()'s CPXchgprobtype(LP).  That relaxation
  // resets EVERY column's ctype to CPX_CONTINUOUS, so the problem-type flip
  // alone does NOT bring integrality back — flipping to MILP/MIQP leaves all
  // columns continuous (is_integer() reads CPX_CONTINUOUS, and a subsequent
  // solve would ignore integrality).  Re-impose CPX_INTEGER on the
  // caller-snapshotted columns in one bulk CPXchgctype call (still O(1)
  // CPLEX round-trips, not per-column).  Binary columns are restored as
  // CPX_INTEGER; their [0,1] bounds already encode binariness.
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type == CPXPROB_LP) {
    CPXchgprobtype(m_env_lp_.env(), m_env_lp_.lp(), CPXPROB_MILP);
  } else if (cplex_type == CPXPROB_QP) {
    CPXchgprobtype(m_env_lp_.env(), m_env_lp_.lp(), CPXPROB_MIQP);
  }
  if (integer_cols.empty()) {
    return 0;
  }
  const std::vector<char> ctypes(integer_cols.size(), CPX_INTEGER);
  CPXchgctype(m_env_lp_.env(),
              m_env_lp_.lp(),
              static_cast<int>(integer_cols.size()),
              integer_cols.data(),
              ctypes.data());
  return static_cast<int>(integer_cols.size());
}

namespace
{
/// Map a CPLEX cstat/rstat code to the solver-agnostic BasisStatus.
[[nodiscard]] BasisStatus from_cplex_status(int cstat) noexcept
{
  switch (cstat) {
    case CPX_BASIC:
      return BasisStatus::basic;
    case CPX_AT_UPPER:
      return BasisStatus::at_upper;
    case CPX_FREE_SUPER:
      return BasisStatus::free;
    case CPX_AT_LOWER:
    default:
      return BasisStatus::at_lower;
  }
}

/// Map a solver-agnostic BasisStatus to the CPLEX cstat/rstat code.
[[nodiscard]] int to_cplex_status(BasisStatus status) noexcept
{
  switch (status) {
    case BasisStatus::basic:
      return CPX_BASIC;
    case BasisStatus::at_upper:
      return CPX_AT_UPPER;
    case BasisStatus::free:
      return CPX_FREE_SUPER;
    case BasisStatus::at_lower:
      return CPX_AT_LOWER;
  }
  return CPX_AT_LOWER;
}
}  // namespace

std::optional<Basis> CplexSolverBackend::get_basis() const
{
  const int ncols = CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp());
  const int nrows = CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp());
  if (ncols <= 0) {
    return std::nullopt;
  }
  std::vector<int> cstat(static_cast<std::size_t>(ncols));
  std::vector<int> rstat(static_cast<std::size_t>(nrows));
  // Nonzero status (e.g. CPXERR_NO_BASIS) → the LP carries no basis (interior
  // point without crossover, or never solved) → report "none".
  if (CPXgetbase(m_env_lp_.env(), m_env_lp_.lp(), cstat.data(), rstat.data())
      != 0)
  {
    return std::nullopt;
  }
  Basis basis;
  basis.col_status.resize(static_cast<std::size_t>(ncols));
  basis.row_status.resize(static_cast<std::size_t>(nrows));
  for (std::size_t i = 0; i < basis.col_status.size(); ++i) {
    basis.col_status[i] = from_cplex_status(cstat[i]);
  }
  for (std::size_t i = 0; i < basis.row_status.size(); ++i) {
    basis.row_status[i] = from_cplex_status(rstat[i]);
  }
  return basis;
}

bool CplexSolverBackend::set_basis(const Basis& basis)
{
  const auto ncols =
      static_cast<std::size_t>(CPXgetnumcols(m_env_lp_.env(), m_env_lp_.lp()));
  const auto nrows =
      static_cast<std::size_t>(CPXgetnumrows(m_env_lp_.env(), m_env_lp_.lp()));
  // The LinearInterface reconciles dimensions before calling; a mismatch here
  // means a misuse (raw backend caller) — reject rather than guess.
  if (basis.col_status.size() != ncols || basis.row_status.size() != nrows) {
    return false;
  }
  std::vector<int> cstat(ncols);
  std::vector<int> rstat(nrows);
  for (std::size_t i = 0; i < ncols; ++i) {
    cstat[i] = to_cplex_status(basis.col_status[i]);
  }
  for (std::size_t i = 0; i < nrows; ++i) {
    rstat[i] = to_cplex_status(basis.row_status[i]);
  }
  // Installs the advanced start; the next CPXlpopt honours it when
  // CPX_PARAM_ADVIND=1 (SolverOptions::advanced_basis).
  return CPXcopybase(
             m_env_lp_.env(), m_env_lp_.lp(), cstat.data(), rstat.data())
      == 0;
}

std::optional<std::vector<std::string>>
CplexSolverBackend::diagnose_infeasibility(int max_items)
{
  auto* env = m_env_lp_.env();
  auto* lp = m_env_lp_.lp();

  const int nrows = CPXgetnumrows(env, lp);
  const int ncols = CPXgetnumcols(env, lp);
  if (nrows <= 0) {
    return std::nullopt;
  }

  // Refine the conflict: CPLEX computes a minimal infeasible subsystem (IIS)
  // of the current (infeasible) problem.  Non-zero rc ⇒ refiner unavailable
  // for this problem; report "unsupported" rather than throwing.
  int confnumrows = 0;
  int confnumcols = 0;
  if (CPXrefineconflict(env, lp, &confnumrows, &confnumcols) != 0) {
    return std::nullopt;
  }

  // Retrieve which rows / cols are members of the conflict.
  int confstat = 0;
  int outrows = 0;
  int outcols = 0;
  std::vector<int> rowind(static_cast<std::size_t>(std::max(nrows, 1)));
  std::vector<int> rowbdstat(static_cast<std::size_t>(std::max(nrows, 1)));
  std::vector<int> colind(static_cast<std::size_t>(std::max(ncols, 1)));
  std::vector<int> colbdstat(static_cast<std::size_t>(std::max(ncols, 1)));
  if (CPXgetconflict(env,
                     lp,
                     &confstat,
                     rowind.data(),
                     rowbdstat.data(),
                     &outrows,
                     colind.data(),
                     colbdstat.data(),
                     &outcols)
      != 0)
  {
    return std::nullopt;
  }

  // Resolve a single row's name (or a synthetic "row_<i>" fallback).
  const auto row_name = [&](int i) -> std::string
  {
    std::array<char, 256> store {};
    char* nameptr = nullptr;
    int surplus = 0;
    const int rc = CPXgetrowname(env,
                                 lp,
                                 &nameptr,
                                 store.data(),
                                 static_cast<int>(store.size()),
                                 &surplus,
                                 i,
                                 i);
    if (rc == 0 && nameptr != nullptr) {
      return std::string {nameptr};
    }
    return std::format("row_{}", i);
  };

  std::vector<std::string> conflicts;
  const int cap = (max_items > 0) ? max_items : outrows;
  conflicts.reserve(static_cast<std::size_t>(std::min(outrows, cap)));
  for (int k = 0; k < outrows && static_cast<int>(conflicts.size()) < cap; ++k)
  {
    if (rowbdstat[static_cast<std::size_t>(k)] == CPX_CONFLICT_MEMBER) {
      conflicts.push_back(row_name(rowind[static_cast<std::size_t>(k)]));
    }
  }
  return conflicts;
}

namespace
{
// Capture a solve's solver-reported wall time (CPXgettime) + deterministic
// ticks (CPXgetdettime) as a per-solve delta on the env.  The generic
// SolveEffort accounting (SolveEffortTotals, accumulated in LinearInterface)
// reads this via last_solve_effort().
[[nodiscard]] SolveEffort capture_cplex_effort(cpxenv* env,
                                               double t0_ticks,
                                               double t0_secs)
{
  SolveEffort e;
  double t1 = 0.0;
  if (CPXgetdettime(env, &t1) == 0) {
    e.ticks = t1 - t0_ticks;
  }
  double s1 = 0.0;
  if (CPXgettime(env, &s1) == 0) {
    e.seconds = s1 - t0_secs;
  }
  return e;
}
}  // namespace

void CplexSolverBackend::initial_solve()
{
  double t0 = 0.0;
  double s0 = 0.0;
  CPXgetdettime(m_env_lp_.env(), &t0);
  CPXgettime(m_env_lp_.env(), &s0);
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    m_solve_status_ = CPXmipopt(m_env_lp_.env(), m_env_lp_.lp());
  } else {
    m_solve_status_ = CPXlpopt(m_env_lp_.env(), m_env_lp_.lp());
  }
  m_last_effort_ = capture_cplex_effort(m_env_lp_.env(), t0, s0);
}

void CplexSolverBackend::resolve()
{
  double t0 = 0.0;
  double s0 = 0.0;
  CPXgetdettime(m_env_lp_.env(), &t0);
  CPXgettime(m_env_lp_.env(), &s0);
  const int cplex_type = CPXgetprobtype(m_env_lp_.env(), m_env_lp_.lp());
  if (cplex_type == CPXPROB_MILP || cplex_type == CPXPROB_MIQP) {
    m_solve_status_ = CPXmipopt(m_env_lp_.env(), m_env_lp_.lp());
  } else {
    // CPXlpopt respects CPX_PARAM_LPMETHOD set by apply_options().
    m_solve_status_ = CPXlpopt(m_env_lp_.env(), m_env_lp_.lp());
  }
  m_last_effort_ = capture_cplex_effort(m_env_lp_.env(), t0, s0);
}

SolveEffort CplexSolverBackend::last_solve_effort() const
{
  return m_last_effort_;
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
      .threads = 0,
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
  // CPXchgcolname / CPXchgrowname walk an internal CPLEX name table on
  // every call, so the previous per-column / per-row loop became O(N^2)
  // and on 7-day CEN PCP cases (~330k cols) spent multiple minutes
  // entirely inside CPLEX before `write_lp` could even open the file.
  // Fix: pack non-empty (index, name) pairs into a single bulk-array
  // call.  Empty entries are skipped to preserve the previous "leave
  // unnamed columns blank" behaviour.
  auto push =
      [&](const std::vector<std::string>& names, auto chg_fn, const char* kind)
  {
    std::vector<int> indices;
    std::vector<char*> name_ptrs;
    std::vector<std::string> name_bufs;
    indices.reserve(names.size());
    name_ptrs.reserve(names.size());
    name_bufs.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i].empty()) {
        continue;
      }
      name_bufs.emplace_back(names[i]);
      indices.push_back(static_cast<int>(i));
    }
    name_ptrs.resize(name_bufs.size());
    for (size_t k = 0; k < name_bufs.size(); ++k) {
      name_ptrs[k] = name_bufs[k].data();
    }
    if (indices.empty()) {
      return;
    }
    const int status = chg_fn(m_env_lp_.env(),
                              m_env_lp_.lp(),
                              static_cast<int>(indices.size()),
                              indices.data(),
                              name_ptrs.data());
    if (status != 0) {
      std::fprintf(stderr,
                   "CPLEX: bulk %s rename failed with status %d\n",
                   kind,
                   status);
    }
  };

  push(col_names, &CPXchgcolname, "column");
  push(row_names, &CPXchgrowname, "row");
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
