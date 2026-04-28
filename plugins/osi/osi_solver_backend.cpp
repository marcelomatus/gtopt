/**
 * @file      osi_solver_backend.cpp
 * @brief     OSI-based solver backend implementation
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <format>
#include <stdexcept>

#include "osi_solver_backend.hpp"

#include <coin/ClpConfig.h>
#include <coin/CoinMessageHandler.hpp>
#include <coin/CoinPackedMatrix.hpp>
#include <coin/CoinPackedVector.hpp>
#include <coin/OsiClpSolverInterface.hpp>
#include <coin/OsiSolverInterface.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/utils.hpp>

#ifdef GTOPT_OSI_HAS_CBC
#  include <coin/OsiCbcSolverInterface.hpp>
#endif

// Check if ClpSimplex is available for CLP-specific optimizations
#include <coin/ClpSimplex.hpp>

namespace gtopt
{

namespace
{

std::shared_ptr<OsiSolverInterface> make_osi_solver(
    OsiSolverBackend::OsiSolverType type)
{
  switch (type) {
    case OsiSolverBackend::OsiSolverType::clp:
      return std::make_shared<OsiClpSolverInterface>();
#ifdef GTOPT_OSI_HAS_CBC
    case OsiSolverBackend::OsiSolverType::cbc:
      return std::make_shared<OsiCbcSolverInterface>();
#endif
    default:
      throw std::runtime_error("Unsupported OSI solver type");
  }
}

/// Try to get the underlying OsiClpSolverInterface for CLP-specific ops
OsiClpSolverInterface* as_clp(OsiSolverInterface* solver,
                              OsiSolverBackend::OsiSolverType type)
{
  if (type == OsiSolverBackend::OsiSolverType::clp) {
    return dynamic_cast<OsiClpSolverInterface*>(solver);
  }
#ifdef GTOPT_OSI_HAS_CBC
  if (type == OsiSolverBackend::OsiSolverType::cbc) {
    auto* cbc = dynamic_cast<OsiCbcSolverInterface*>(solver);
    if (cbc != nullptr) {
      return dynamic_cast<OsiClpSolverInterface*>(cbc->getRealSolverPtr());
    }
  }
#endif
  return nullptr;
}

/// Apply every SolverOptions field onto a *fresh* OsiSolverInterface.
///
/// Pure helper: mutates `solver` only, touches no backend members.  Shared
/// between the live `apply_options()` path and `reset_solver_()`, so any
/// option the caller ever set is replayed onto the new solver on every
/// load_problem() cycle and on clone().
///
/// NOTE: SolverOptions::memory_emphasis has no documented COIN/CLP
/// equivalent.  We deliberately leave it as a no-op here rather than
/// forcing CLP's "maximizePivots(0)" or similar tweaks that would slow
/// down all solves.
void apply_options_to_solver(OsiSolverInterface* solver,
                             OsiSolverBackend::OsiSolverType type,
                             const SolverOptions& opts)
{
  if (solver == nullptr) {
    return;
  }

  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    solver->setDblParam(OsiDualTolerance, *oeps);
  }
  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    solver->setDblParam(OsiPrimalTolerance, *feps);
  }

  // Time limit (CLP supports this natively via ClpSimplex)
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    auto* clp = as_clp(solver, type);
    if (clp != nullptr) {
      clp->getModelPtr()->setMaximumSeconds(*tl);
    }
  }

  // CLP scaling: 0=off, 2=geometric, 3=auto(default).
  if (opts.scaling.has_value()) {
    auto* clp = as_clp(solver, type);
    if (clp != nullptr) {
      auto* clp_model = clp->getModelPtr();
      if (clp_model != nullptr) {
        int mode = 3;  // auto (CLP default)
        switch (*opts.scaling) {
          case SolverScaling::none:
            mode = 0;
            break;
          case SolverScaling::automatic:
            mode = 3;
            break;
          case SolverScaling::aggressive:
            mode = 2;
            break;
        }
        clp_model->scaling(mode);
      }
    }
  }

  solver->setHintParam(OsiDoPresolveInInitial, opts.presolve, OsiHintDo);

  constexpr bool On = true;
  constexpr bool Off = false;

  switch (opts.algorithm) {
    case LPAlgo::default_algo:
      break;
    case LPAlgo::primal:
      solver->setHintParam(OsiDoDualInInitial, Off, OsiHintDo);
      solver->setHintParam(OsiDoDualInResolve, Off, OsiHintDo);
      break;
    case LPAlgo::dual:
      solver->setHintParam(OsiDoDualInInitial, On, OsiHintDo);
      solver->setHintParam(OsiDoDualInResolve, On, OsiHintDo);
      break;
    case LPAlgo::barrier: {
      auto* clp = as_clp(solver, type);
      if (clp != nullptr) {
        auto* clp_model = clp->getModelPtr();
        if (clp_model != nullptr) {
          clp_model->setAlgorithm(-1);  // -1 = barrier
          if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
            clp_model->setDblParam(ClpDualTolerance, *beps);
          }
        }
      }
      solver->setHintParam(OsiDoDualInInitial, Off, OsiHintDo);
      solver->setHintParam(OsiDoDualInResolve, Off, OsiHintDo);
      break;
    }
    case LPAlgo::last_algo:
      break;
  }
}

}  // namespace

OsiSolverBackend::OsiSolverBackend(OsiSolverType type)
    : m_type_(type)
    , m_solver_(make_osi_solver(type))
    , m_handler_(std::make_unique<CoinMessageHandler>())
{
  m_handler_->setLogLevel(0);
  m_solver_->passInMessageHandler(m_handler_.get());
  m_solver_->setIntParam(OsiNameDiscipline, 0);
}

OsiSolverBackend::OsiSolverBackend(OsiSolverType type,
                                   std::shared_ptr<OsiSolverInterface> solver)
    : m_type_(type)
    , m_solver_(std::move(solver))
    , m_handler_(std::make_unique<CoinMessageHandler>())
{
  m_handler_->setLogLevel(0);
  m_solver_->passInMessageHandler(m_handler_.get());
}

OsiSolverBackend::~OsiSolverBackend() = default;

std::string_view OsiSolverBackend::solver_name() const noexcept
{
  switch (m_type_) {
    case OsiSolverType::clp:
      return "clp";
    case OsiSolverType::cbc:
      return "cbc";
  }
  return "osi";
}

std::string OsiSolverBackend::solver_version() const
{
  return CLP_VERSION;
}

double OsiSolverBackend::infinity() const noexcept
{
  return m_solver_->getInfinity();
}

bool OsiSolverBackend::supports_mip() const noexcept
{
  // CLP is a pure LP solver; CBC is the COIN-OR MIP solver.
  return m_type_ == OsiSolverType::cbc;
}

void OsiSolverBackend::set_prob_name(const std::string& name)
{
  m_prep_.prob_name = name;
  m_solver_->setStrParam(OsiProbName, name);
}

std::string OsiSolverBackend::get_prob_name() const
{
  std::string name;
  return m_solver_->getStrParam(OsiProbName, name) ? name : "";
}

void OsiSolverBackend::reset_solver_()
{
  // Replace the OSI solver instance with a fresh one and replay every cached
  // piece of backend state (options, log, prob_name).  Mirrors the CPLEX
  // plugin's reset_env_lp() and guarantees load_problem() starts from a
  // clean solver each time.
  m_solver_ = make_osi_solver(m_type_);
  m_handler_ = std::make_unique<CoinMessageHandler>();
  m_handler_->setLogLevel(0);
  m_solver_->passInMessageHandler(m_handler_.get());
  m_solver_->setIntParam(OsiNameDiscipline, 0);

  if (!m_prep_.prob_name.empty()) {
    m_solver_->setStrParam(OsiProbName, m_prep_.prob_name);
  }
  if (m_prep_.options.has_value()) {
    apply_options_to_solver(m_solver_.get(), m_type_, *m_prep_.options);
  }
  if (m_prep_.log_level > 0 && !m_prep_.log_filename.empty()) {
    const auto log_path = std::format("{}.log", m_prep_.log_filename);
    m_log_file_ptr_.reset(
        std::fopen(  // NOLINT(cppcoreguidelines-owning-memory)
            log_path.c_str(),
            "ae"));
    if (m_log_file_ptr_) {
      m_handler_ = std::make_unique<CoinMessageHandler>(m_log_file_ptr_.get());
      m_handler_->setLogLevel(m_prep_.log_level);
      m_solver_->passInMessageHandler(m_handler_.get());
    }
  }
}

void OsiSolverBackend::load_problem(int ncols,
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
  reset_solver_();
  m_solver_->loadProblem(
      ncols, nrows, matbeg, matind, matval, collb, colub, obj, rowlb, rowub);
}

int OsiSolverBackend::get_num_cols() const
{
  return m_solver_->getNumCols();
}

int OsiSolverBackend::get_num_rows() const
{
  return m_solver_->getNumRows();
}

void OsiSolverBackend::add_col(double lb, double ub, double obj)
{
  const CoinPackedVector empty_vec;
  m_solver_->addCol(empty_vec, lb, ub, obj);
}

void OsiSolverBackend::set_col_lower(int index, double value)
{
  m_solver_->setColLower(index, value);
}

void OsiSolverBackend::set_col_upper(int index, double value)
{
  m_solver_->setColUpper(index, value);
}

void OsiSolverBackend::set_obj_coeff(int index, double value)
{
  m_solver_->setObjCoeff(index, value);
}

void OsiSolverBackend::add_row(int num_elements,
                               const int* columns,
                               const double* elements,
                               double rowlb,
                               double rowub)
{
  m_solver_->addRow(num_elements, columns, elements, rowlb, rowub);
}

void OsiSolverBackend::add_rows(int num_rows,
                                const int* rowbeg,
                                const int* rowind,
                                const double* rowval,
                                const double* rowlb,
                                const double* rowub)
{
  // OSI does not have a CSR bulk addRows, so dispatch per row.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (const int r : iota_range(0, num_rows)) {
    const int start = rowbeg[r];
    const int count = rowbeg[r + 1] - start;
    m_solver_->addRow(
        count, rowind + start, rowval + start, rowlb[r], rowub[r]);
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

void OsiSolverBackend::set_row_lower(int index, double value)
{
  m_solver_->setRowLower(index, value);
}

void OsiSolverBackend::set_row_upper(int index, double value)
{
  m_solver_->setRowUpper(index, value);
}

void OsiSolverBackend::set_row_bounds(int index, double lb, double ub)
{
  m_solver_->setRowBounds(index, lb, ub);
}

void OsiSolverBackend::delete_rows(int num, const int* indices)
{
  m_solver_->deleteRows(num, indices);
}

double OsiSolverBackend::get_coeff(int row, int col) const
{
  const auto* matrix = m_solver_->getMatrixByCol();
  if (matrix == nullptr) {
    return 0.0;
  }
  return matrix->getCoefficient(row, col);
}

void OsiSolverBackend::set_coeff(int row, int col, double value)
{
  switch (m_type_) {
    case OsiSolverType::clp: {
      auto* clp = dynamic_cast<OsiClpSolverInterface*>(m_solver_.get());
      if (clp != nullptr) {
        clp->modifyCoefficient(row, col, value, false);
      }
      break;
    }
    case OsiSolverType::cbc: {
      auto* clp = as_clp(m_solver_.get(), m_type_);
      if (clp != nullptr) {
        clp->modifyCoefficient(row, col, value, false);
      }
      break;
    }
  }
}

bool OsiSolverBackend::supports_set_coeff() const noexcept
{
  return true;  // All OSI solvers we support have this capability
}

void OsiSolverBackend::set_continuous(int index)
{
  m_solver_->setContinuous(index);
}

void OsiSolverBackend::set_integer(int index)
{
  m_solver_->setInteger(index);
}

bool OsiSolverBackend::is_continuous(int index) const
{
  return m_solver_->isContinuous(index);
}

bool OsiSolverBackend::is_integer(int index) const
{
  return m_solver_->isInteger(index);
}

const double* OsiSolverBackend::col_lower() const
{
  return m_solver_->getColLower();
}

const double* OsiSolverBackend::col_upper() const
{
  return m_solver_->getColUpper();
}

const double* OsiSolverBackend::obj_coefficients() const
{
  return m_solver_->getObjCoefficients();
}

const double* OsiSolverBackend::row_lower() const
{
  return m_solver_->getRowLower();
}

const double* OsiSolverBackend::row_upper() const
{
  return m_solver_->getRowUpper();
}

const double* OsiSolverBackend::col_solution() const
{
  return m_solver_->getColSolution();
}

const double* OsiSolverBackend::reduced_cost() const
{
  return m_solver_->getReducedCost();
}

const double* OsiSolverBackend::row_price() const
{
  return m_solver_->getRowPrice();
}

double OsiSolverBackend::obj_value() const
{
  return m_solver_->getObjValue();
}

void OsiSolverBackend::set_col_solution(const double* sol)
{
  m_solver_->setColSolution(sol);
}

void OsiSolverBackend::set_row_price(const double* price)
{
  m_solver_->setRowPrice(price);
}

void OsiSolverBackend::initial_solve()
{
  m_solver_->initialSolve();
}

void OsiSolverBackend::resolve()
{
#ifdef GTOPT_OSI_HAS_CBC
  // When running CBC with integer variables, invoke branch-and-bound.
  // OsiCbcSolverInterface::resolve() only does an LP re-solve; the MIP
  // solver requires an explicit branchAndBound() call.
  if (m_type_ == OsiSolverType::cbc && m_solver_->getNumIntegers() > 0) {
    m_solver_->branchAndBound();
    return;
  }
#endif
  m_solver_->resolve();
}

void OsiSolverBackend::engage_robust_solve()
{
  if (m_solver_ == nullptr) {
    return;
  }

  if (!m_saved_robust_state_.has_value()) {
    RobustState saved {};
    double dt = 1e-7;
    double pt = 1e-7;
    m_solver_->getDblParam(OsiDualTolerance, dt);
    m_solver_->getDblParam(OsiPrimalTolerance, pt);
    saved.dual_tolerance = dt;
    saved.primal_tolerance = pt;
    saved.presolve_passes = m_presolve_ ? 1 : 0;
    saved.engage_count = 0;
    m_saved_robust_state_ = saved;
  }
  ++m_saved_robust_state_->engage_count;

  // Compound the loosening — read live values, multiply by 10, clamp to
  // 1e-1 (CLP rejects tolerances above this).
  constexpr double k_max_tol = 1e-1;
  double cur_dt = m_saved_robust_state_->dual_tolerance;
  double cur_pt = m_saved_robust_state_->primal_tolerance;
  m_solver_->getDblParam(OsiDualTolerance, cur_dt);
  m_solver_->getDblParam(OsiPrimalTolerance, cur_pt);

  m_solver_->setDblParam(OsiDualTolerance, std::min(cur_dt * 10.0, k_max_tol));
  m_solver_->setDblParam(OsiPrimalTolerance,
                         std::min(cur_pt * 10.0, k_max_tol));

  // Force presolve on — CLP's closest analogue to CPLEX REPEATPRESOLVE.
  m_solver_->setHintParam(OsiDoPresolveInInitial, true, OsiHintDo);
  m_solver_->setHintParam(OsiDoPresolveInResolve, true, OsiHintDo);

  // CLP-specific: switch to dual simplex with cranked-up iteration cap
  // to give the resolve more chances to recover from degeneracy.
  if (auto* clp = as_clp(m_solver_.get(), m_type_); clp != nullptr) {
    if (auto* clp_model = clp->getModelPtr(); clp_model != nullptr) {
      clp_model->scaling(2);  // geometric — more aggressive than auto
    }
  }
}

void OsiSolverBackend::disengage_robust_solve() noexcept
{
  if (!m_saved_robust_state_.has_value()) {
    return;
  }
  if (m_solver_ == nullptr) {
    m_saved_robust_state_.reset();
    return;
  }

  // OSI's setHintParam can throw CoinError on invalid hint strength; wrap
  // every call so the noexcept contract is honoured even if a backend
  // rejects a setting on restore.
  try {
    const auto& s = *m_saved_robust_state_;
    m_solver_->setDblParam(OsiDualTolerance, s.dual_tolerance);
    m_solver_->setDblParam(OsiPrimalTolerance, s.primal_tolerance);
    m_solver_->setHintParam(
        OsiDoPresolveInInitial, s.presolve_passes > 0, OsiHintDo);
    m_solver_->setHintParam(
        OsiDoPresolveInResolve, s.presolve_passes > 0, OsiHintDo);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Best-effort restore — swallow exceptions to keep noexcept.
  }
  m_saved_robust_state_.reset();
}

bool OsiSolverBackend::is_proven_optimal() const
{
  return m_solver_->isProvenOptimal();
}

bool OsiSolverBackend::is_abandoned() const
{
  return m_solver_->isAbandoned();
}

bool OsiSolverBackend::is_proven_primal_infeasible() const
{
  return m_solver_->isProvenPrimalInfeasible();
}

bool OsiSolverBackend::is_proven_dual_infeasible() const
{
  return m_solver_->isProvenDualInfeasible();
}

LPAlgo OsiSolverBackend::get_algorithm() const
{
  return m_algorithm_;
}

int OsiSolverBackend::get_threads() const
{
  return m_threads_;
}

bool OsiSolverBackend::get_presolve() const
{
  return m_presolve_;
}

int OsiSolverBackend::get_log_level() const
{
  return m_log_level_;
}

SolverOptions OsiSolverBackend::optimal_options() const
{
  return {
      .algorithm = LPAlgo::dual,
      .threads = 1,
      .presolve = true,
      .scaling = SolverScaling::automatic,
      .max_fallbacks = 2,
  };
}

void OsiSolverBackend::apply_options(const SolverOptions& opts)
{
  m_prep_.options = opts;
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;

  apply_options_to_solver(m_solver_.get(), m_type_, opts);
}

std::optional<double> OsiSolverBackend::get_kappa() const
{
  // Return the largest dual error as a rough proxy for the condition
  // number.  ClpFactorization::conditionNumber() is only available when
  // CLP is built without CLP_MULTIPLE_FACTORIZATIONS; in multi-factorization
  // builds (the default on Ubuntu) the method is absent — hence the
  // dual-error proxy.  Any failure path returns std::nullopt so callers
  // do not fold a bogus sentinel into aggregate statistics.
  auto* clp =
      as_clp(const_cast<OsiSolverInterface*>(m_solver_.get()),  // NOLINT
             m_type_);
  if (clp != nullptr) {
    try {
      auto* model = clp->getModelPtr();
      if (model != nullptr) {
        return model->largestDualError();
      }
    } catch (...) {  // NOLINT(bugprone-empty-catch)
      // CLP may throw on degenerate or empty models; treat as unavailable.
    }
  }
  return std::nullopt;
}

void OsiSolverBackend::open_log(FILE* file, int level)
{
  if (file == nullptr) {
    return;
  }
  m_handler_ = std::make_unique<CoinMessageHandler>(file);
  m_handler_->setLogLevel(level);
  m_solver_->passInMessageHandler(m_handler_.get());
}

void OsiSolverBackend::close_log()
{
  m_handler_ = std::make_unique<CoinMessageHandler>();
  m_handler_->setLogLevel(0);
  m_solver_->passInMessageHandler(m_handler_.get());
}

void OsiSolverBackend::set_log_filename(const std::string& filename, int level)
{
  m_prep_.log_filename = filename;
  m_prep_.log_level = level;
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    m_log_file_ptr_.reset(
        std::fopen(  // NOLINT(cppcoreguidelines-owning-memory)
            log_path.c_str(),
            "ae"));
    if (!m_log_file_ptr_) {
      throw std::runtime_error(std::format(
          "failed to open solver log file {}: errno {}", log_path, errno));
    }
    m_handler_ = std::make_unique<CoinMessageHandler>(m_log_file_ptr_.get());
    m_handler_->setLogLevel(level);
    m_solver_->passInMessageHandler(m_handler_.get());
  }
}

void OsiSolverBackend::clear_log_filename()
{
  m_prep_.log_filename.clear();
  m_prep_.log_level = 0;
  m_handler_ = std::make_unique<CoinMessageHandler>();
  m_handler_->setLogLevel(0);
  m_solver_->passInMessageHandler(m_handler_.get());
  if (m_log_file_ptr_) {
    (void)std::fflush(m_log_file_ptr_.get());
    m_log_file_ptr_.reset();
  }
}

namespace
{

/// CoinLpIO's name validator rejects:
///   - empty names (gaps in the name vector);
///   - names with characters outside `[A-Za-z0-9_.]` — notably `-`,
///     which appears in gtopt's unknown-uid placeholder `_-1_...`.
/// When *any* name fails validation CoinLpIO discards the *entire*
/// set and falls back to `R1, R2, ...`, breaking LP-file auditing
/// and every downstream tool that relies on the generated labels.
///
/// `sanitise_lp_name` replaces invalid characters with `_` and
/// substitutes a positional placeholder (`prefix_<idx>`) for empty
/// names.  The sanitised form only affects the OSI backend's name
/// storage; authoritative names on `LinearInterface` are preserved.
[[nodiscard]] std::string sanitise_lp_name(std::string_view raw,
                                           std::string_view prefix,
                                           std::size_t idx)
{
  if (raw.empty()) {
    // Synthesise a deterministic placeholder so CoinLpIO accepts the
    // full name vector.  Index-based so two gaps can't collide.
    return std::format("{}{}", prefix, idx);
  }
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '.';
    out.push_back(ok ? c : '_');
  }
  return out;
}

}  // namespace

void OsiSolverBackend::push_names(const std::vector<std::string>& col_names,
                                  const std::vector<std::string>& row_names)
{
  // Two name stores are in play on OsiClpSolverInterface:
  //   1. ClpModel's own `m_rowNames_` / `m_columnNames_` (CLP-internal
  //      solver consumers).
  //   2. OSI's base-class name vectors, gated by `OsiNameDiscipline`
  //      (what `OsiSolverInterface::writeLp` actually reads from).
  //
  // A previous `copyNames`-only fast path populated only (1), so cut
  // rows added after `load_flat` had their names ignored by the LP
  // file writer — the generated .lp file carried `R1, R2, ...`
  // defaults instead of `sddp_fcut_...` / `sddp_bcut_...`.  Always
  // populate BOTH stores: the CLP bulk copy for solver-internal
  // consumers, and the OSI per-element path so writeLp() emits real
  // labels.  Both passes are O(ncols + nrows) and negligible next to
  // the solve they precede.
  //
  // We also sanitise names for the OSI store: CoinLpIO's writeLp
  // rejects the whole set if any name contains characters it deems
  // "illegal" (notably `-` in gtopt's `-1_` unknown-uid placeholder),
  // silently falling back to `R1, R2, ...`.  The sanitised form only
  // affects what OSI writes to `.lp` files; `LinearInterface`'s own
  // `m_row_index_to_name_` / `m_col_index_to_name_` maps keep the
  // original names verbatim.
  m_safe_col_names_.clear();
  m_safe_row_names_.clear();
  m_safe_col_names_.reserve(col_names.size());
  m_safe_row_names_.reserve(row_names.size());
  for (std::size_t i = 0; i < col_names.size(); ++i) {
    m_safe_col_names_.push_back(sanitise_lp_name(col_names[i], "c", i));
  }
  for (std::size_t i = 0; i < row_names.size(); ++i) {
    m_safe_row_names_.push_back(sanitise_lp_name(row_names[i], "r", i));
  }

  if (auto* clp = as_clp(m_solver_.get(), m_type_); clp != nullptr) {
    clp->getModelPtr()->copyNames(m_safe_row_names_, m_safe_col_names_);
  }

  m_solver_->setIntParam(OsiNameDiscipline, 2);
  for (std::size_t i = 0; i < m_safe_col_names_.size(); ++i) {
    m_solver_->setColName(static_cast<int>(i), m_safe_col_names_[i]);
  }
  for (std::size_t i = 0; i < m_safe_row_names_.size(); ++i) {
    m_solver_->setRowName(static_cast<int>(i), m_safe_row_names_[i]);
  }
}

void OsiSolverBackend::write_lp(const char* filename)
{
  // CoinLpIO::setLpDataRowAndColNames validates rownames[nrow] as the
  // objective function name slot.  OsiSolverInterface::getObjName()
  // returns "" by default, which CoinLpIO treats as an invalid name and
  // falls back to default "cons0/cons1/..." row labels — erasing all
  // custom constraint names (including "sddp_fcut_...") from the LP file.
  // Ensure the objective has a non-empty name before writing.
  if (m_solver_->getObjName().empty()) {
    m_solver_->setObjName("obj");
  }
  m_solver_->writeLp(filename);
}

std::unique_ptr<SolverBackend> OsiSolverBackend::clone() const
{
  auto* raw = m_solver_->clone(true);
  auto* concrete = dynamic_cast<OsiSolverInterface*>(raw);
  if (concrete == nullptr) {
    delete raw;  // NOLINT(cppcoreguidelines-owning-memory)
    return std::make_unique<OsiSolverBackend>(m_type_);
  }
  std::shared_ptr<OsiSolverInterface> cloned_solver(concrete);
  auto cloned =
      std::make_unique<OsiSolverBackend>(m_type_, std::move(cloned_solver));

  // Replay every cached field so the clone owns a backend indistinguishable
  // from this one after a load_problem() cycle.  Options are applied to the
  // freshly-cloned OSI solver via the shared helper; log + prob_name are
  // replayed through the public setters so the clone owns its own FILE*.
  cloned->m_prep_ = m_prep_;
  cloned->m_algorithm_ = m_algorithm_;
  cloned->m_threads_ = m_threads_;
  cloned->m_presolve_ = m_presolve_;
  cloned->m_log_level_ = m_log_level_;

  if (!cloned->m_prep_.prob_name.empty()) {
    cloned->m_solver_->setStrParam(OsiProbName, cloned->m_prep_.prob_name);
  }
  if (cloned->m_prep_.options.has_value()) {
    apply_options_to_solver(
        cloned->m_solver_.get(), m_type_, *cloned->m_prep_.options);
  }
  if (cloned->m_prep_.log_level > 0 && !cloned->m_prep_.log_filename.empty()) {
    cloned->set_log_filename(cloned->m_prep_.log_filename,
                             cloned->m_prep_.log_level);
  }

  return cloned;
}

}  // namespace gtopt
