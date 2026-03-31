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

void OsiSolverBackend::set_prob_name(const std::string& name)
{
  m_solver_->setStrParam(OsiProbName, name);
}

std::string OsiSolverBackend::get_prob_name() const
{
  std::string name;
  return m_solver_->getStrParam(OsiProbName, name) ? name : "";
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
  m_solver_->resolve();
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

void OsiSolverBackend::apply_options(const SolverOptions& opts)
{
  m_algorithm_ = opts.algorithm;
  m_threads_ = opts.threads;
  m_presolve_ = opts.presolve;
  m_log_level_ = opts.log_level;
  if (const auto oeps = opts.optimal_eps; oeps && *oeps > 0) {
    m_solver_->setDblParam(OsiDualTolerance, *oeps);
  }

  if (const auto feps = opts.feasible_eps; feps && *feps > 0) {
    m_solver_->setDblParam(OsiPrimalTolerance, *feps);
  }

  // Time limit (CLP supports this natively via ClpSimplex)
  if (const auto tl = opts.time_limit; tl && *tl > 0.0) {
    auto* clp = as_clp(m_solver_.get(), m_type_);
    if (clp != nullptr) {
      clp->getModelPtr()->setMaximumSeconds(*tl);
    }
  }

  // ── Warm-start override (skip when barrier is requested) ──
  if (opts.reuse_basis && opts.algorithm != LPAlgo::barrier) {
    m_algorithm_ = LPAlgo::dual;
    m_presolve_ = false;

    m_solver_->setHintParam(OsiDoPresolveInInitial, false, OsiHintDo);
    m_solver_->setHintParam(OsiDoPresolveInResolve, false, OsiHintDo);
    m_solver_->setHintParam(OsiDoDualInInitial, true, OsiHintDo);
    m_solver_->setHintParam(OsiDoDualInResolve, true, OsiHintDo);

    // Force dual simplex on CLP (avoid barrier for warm-started resolves)
    auto* clp = as_clp(m_solver_.get(), m_type_);
    if (clp != nullptr) {
      auto* clp_model = clp->getModelPtr();
      if (clp_model != nullptr) {
        clp_model->setAlgorithm(1);  // 1 = dual simplex
        // Bit 1: keep factorization, Bit 8: keep work areas
        constexpr unsigned keep_factorization = 1U;
        constexpr unsigned keep_work_areas = 8U;
        clp_model->setSpecialOptions(
            static_cast<int>(clp_model->specialOptions() | keep_factorization
                             | keep_work_areas));
      }
    }
    return;
  }

  // CLP scaling: 0=off, 2=geometric, 3=auto(default).
  if (opts.scaling.has_value()) {
    auto* clp = as_clp(m_solver_.get(), m_type_);
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

  const auto presolve = opts.presolve;
  m_solver_->setHintParam(OsiDoPresolveInInitial, presolve, OsiHintDo);

  constexpr bool On = true;
  constexpr bool Off = false;

  switch (opts.algorithm) {
    case LPAlgo::default_algo:
      break;
    case LPAlgo::primal:
      m_solver_->setHintParam(OsiDoDualInInitial, Off, OsiHintDo);
      m_solver_->setHintParam(OsiDoDualInResolve, Off, OsiHintDo);
      break;
    case LPAlgo::dual:
      m_solver_->setHintParam(OsiDoDualInInitial, On, OsiHintDo);
      m_solver_->setHintParam(OsiDoDualInResolve, On, OsiHintDo);
      break;
    case LPAlgo::barrier: {
      // CLP barrier via direct API
      auto* clp = as_clp(m_solver_.get(), m_type_);
      if (clp != nullptr) {
        auto* clp_model = clp->getModelPtr();
        if (clp_model != nullptr) {
          // Use barrier algorithm
          clp_model->setAlgorithm(-1);  // -1 = barrier

          if (const auto beps = opts.barrier_eps; beps && *beps > 0) {
            clp_model->setDblParam(ClpDualTolerance, *beps);
          }
        }
      }
      // Also set hint params for non-CLP solvers
      m_solver_->setHintParam(OsiDoDualInInitial, Off, OsiHintDo);
      m_solver_->setHintParam(OsiDoDualInResolve, Off, OsiHintDo);
      break;
    }
    case LPAlgo::last_algo:
      break;
  }
}

double OsiSolverBackend::get_kappa() const
{
  // Return the largest dual error as a rough proxy for the condition
  // number.  ClpFactorization::conditionNumber() is only available when
  // CLP is built without CLP_MULTIPLE_FACTORIZATIONS; in multi-factorization
  // builds (the default on Ubuntu) the method is absent.
  auto* clp =
      as_clp(const_cast<OsiSolverInterface*>(m_solver_.get()),  // NOLINT
             m_type_);
  if (clp != nullptr) {
    try {
      auto* model = clp->getModelPtr();
      if (model != nullptr) {
        return model->largestDualError();
      }
    } catch (...) {
    }
  }
  return 1.0;
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
  if (level > 0 && !filename.empty()) {
    const auto log_path = std::format("{}.log", filename);
    m_log_file_ptr_.reset(std::fopen(log_path.c_str(), "ae"));
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
  m_handler_ = std::make_unique<CoinMessageHandler>();
  m_handler_->setLogLevel(0);
  m_solver_->passInMessageHandler(m_handler_.get());
  if (m_log_file_ptr_) {
    (void)std::fflush(m_log_file_ptr_.get());
    m_log_file_ptr_.reset();
  }
}

void OsiSolverBackend::push_names(const std::vector<std::string>& col_names,
                                  const std::vector<std::string>& row_names)
{
  // Fast path for CLP: bulk set via ClpModel::copyNames()
  auto* clp = as_clp(m_solver_.get(), m_type_);
  if (clp != nullptr) {
    clp->getModelPtr()->copyNames(row_names, col_names);
    return;
  }

  // Generic fallback: per-element via OSI
  m_solver_->setIntParam(OsiNameDiscipline, 2);
  for (size_t i = 0; i < col_names.size(); ++i) {
    if (!col_names[i].empty()) {
      m_solver_->setColName(static_cast<int>(i), col_names[i]);
    }
  }
  for (size_t i = 0; i < row_names.size(); ++i) {
    if (!row_names[i].empty()) {
      m_solver_->setRowName(static_cast<int>(i), row_names[i]);
    }
  }
}

void OsiSolverBackend::write_lp(const char* filename)
{
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
  std::shared_ptr<OsiSolverInterface> cloned(concrete);
  return std::make_unique<OsiSolverBackend>(m_type_, std::move(cloned));
}

}  // namespace gtopt
