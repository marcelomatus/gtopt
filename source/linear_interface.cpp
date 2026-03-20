#include <algorithm>
#include <cerrno>
#include <expected>
#include <memory>
#include <ranges>

#include <coin/CoinPackedMatrix.hpp>
#include <coin/CoinPackedVector.hpp>
#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <spdlog/spdlog.h>

#ifdef COIN_USE_CLP
#  include <coin/ClpSimplex.hpp>
#endif

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
                              int level)
{
  if (level < 1 || name.empty()) {
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

void LinearInterface::set_prob_name(const std::string& pname)
{
  solver->setStrParam(OsiProbName, pname);
}

std::string LinearInterface::get_prob_name() const
{
  std::string name;
  return solver->getStrParam(OsiProbName, name) ? name : "";
}

void LinearInterface::set_log_file(const std::string& plog_file)
{
  log_file = plog_file;
}

void LinearInterface::close_log_handler()
{
  if (log_file.empty()) {
    return;
  }

  auto new_handler = std::make_unique<CoinMessageHandler>();
  new_handler->setLogLevel(0);
  solver->passInMessageHandler(new_handler.get());
  handler = std::move(new_handler);

  (void)std::fflush(log_file_ptr.get());
}

void LinearInterface::open_log_handler(const int log_level)
{
  if (log_file.empty()) {
    return;
  }

  if (!log_file_ptr) {
    auto file = std::format("{}.log", log_file);
    log_file_ptr = log_file_ptr_t(std::fopen(file.c_str(), "ae"));

    if (!log_file_ptr) {
      throw std::runtime_error(std::format(
          "failed to open solver log file {} : errno {}", log_file, errno));
    }
  }

  auto new_handler = std::make_unique<CoinMessageHandler>(log_file_ptr.get());
  new_handler->setLogLevel(log_level);
  solver->passInMessageHandler(new_handler.get());
  handler = std::move(new_handler);
}

LinearInterface::LinearInterface(solver_ptr_t psolver, std::string plog_file)
    : solver(std::move(psolver))
    , log_file(std::move(plog_file))
    , handler(std::make_unique<CoinMessageHandler>())
{
  handler->setLogLevel(0);
  solver->passInMessageHandler(handler.get());
  solver->setIntParam(OsiNameDiscipline, 2);
}

LinearInterface::LinearInterface(const std::string& plog_file)
    : LinearInterface(std::make_shared<SolverInterface>(), plog_file)
{
}

LinearInterface::LinearInterface(const FlatLinearProblem& flat_lp,
                                 const std::string& plog_file)
    : LinearInterface(std::make_shared<SolverInterface>(), plog_file)
{
  load_flat(flat_lp);
}

LinearInterface LinearInterface::clone() const
{
  // OsiSolverInterface::clone(copyData=true) deep-copies the full LP
  // state including variables, constraints, bounds, objective, and basis.
  // The clone returns OsiSolverInterface* (virtual base), so we need
  // dynamic_cast to recover the concrete solver type.
  auto* raw = solver->clone(true);
  solver_ptr_t cloned_solver(dynamic_cast<SolverInterface*>(raw));
  if (!cloned_solver) {
    // The concrete solver type differs from SolverInterface.
    // Delete the raw pointer to avoid a leak and fall back to a fresh
    // (empty) solver — the caller should re-load data if needed.
    delete raw;  // NOLINT(cppcoreguidelines-owning-memory)
    cloned_solver = std::make_shared<SolverInterface>();
  }
  auto cloned = LinearInterface {std::move(cloned_solver), log_file};
  cloned.m_col_scales_ = m_col_scales_;
  return cloned;
}

void LinearInterface::set_warm_start_solution(
    const std::span<const double> col_sol,
    const std::span<const double> row_dual)
{
  if (!col_sol.empty()) {
    const auto ncols = static_cast<std::size_t>(get_numcols());
    if (col_sol.size() == ncols) {
      set_col_sol(col_sol);
    } else if (col_sol.size() < ncols) {
      // Saved solution has fewer columns (e.g. elastic slacks added later).
      std::vector<double> padded(ncols, 0.0);
      std::ranges::copy(col_sol, padded.begin());
      set_col_sol(padded);
    }
    // col_sol.size() > ncols → stale snapshot, silently skip.
  }
  if (!row_dual.empty()) {
    const auto nrows = static_cast<std::size_t>(get_numrows());
    if (row_dual.size() == nrows) {
      set_row_dual(row_dual);
    } else if (row_dual.size() < nrows) {
      // Saved dual has fewer rows (e.g. Benders cuts added later).
      std::vector<double> padded(nrows, 0.0);
      std::ranges::copy(row_dual, padded.begin());
      set_row_dual(padded);
    }
    // row_dual.size() > nrows → stale snapshot, silently skip.
  }
}

void LinearInterface::load_flat(const FlatLinearProblem& flat_lp)
{
  solver->setStrParam(OsiProbName, flat_lp.name);

  solver->loadProblem(flat_lp.ncols,
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

  // Preserve coefficient statistics computed during to_flat().
  m_stats_nnz_ = flat_lp.stats_nnz;
  m_stats_zeroed_ = flat_lp.stats_zeroed;
  m_stats_max_abs_ = flat_lp.stats_max_abs;
  m_stats_min_abs_ = flat_lp.stats_min_abs;
  m_stats_max_col_ = flat_lp.stats_max_col;
  m_stats_min_col_ = flat_lp.stats_min_col;
  m_stats_max_col_name_ = flat_lp.stats_max_col_name;
  m_stats_min_col_name_ = flat_lp.stats_min_col_name;

  for (auto i : flat_lp.colint) {
    solver->setInteger(i);
  }

  if (m_lp_names_level_ >= 1) {
    m_col_index_to_name_.resize(static_cast<size_t>(flat_lp.ncols),
                                std::string {});
  }
  for (int i = 0; auto&& name : flat_lp.colnm) {
    solver->setColName(i, name);
    if (m_lp_names_level_ >= 1 && !name.empty()) {
      m_col_names_.try_emplace(name, i);
      m_col_index_to_name_[static_cast<size_t>(i)] = name;
    }
    ++i;
  }

  for (int i = 0; auto&& name : flat_lp.rownm) {
    solver->setRowName(i, name);
    if (m_lp_names_level_ >= 1 && !name.empty()) {
      m_row_names_.try_emplace(name, i);
    }
    ++i;
  }
}

#ifdef OSI_EXTENDED
void LinearInterface::set_time_limit(double time_limit)
{
  solver->setDblParam(OsiTimeLimit, time_limit);
}
#else
void LinearInterface::set_time_limit(double /*time_limit*/) {}
#endif

ColIndex LinearInterface::add_col(const std::string& name,
                                  double collb,
                                  double colub)
{
  const auto index = solver->getNumCols();
  check_name_unique(m_col_names_, name, index, "column", m_lp_names_level_);

  const CoinPackedVector vec;
  const double obj = 0;

  solver->addCol(vec, collb, colub, obj, name);

  // Keep inverse map in sync
  if (m_lp_names_level_ >= 1 && !name.empty()) {
    if (m_col_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_col_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_col_index_to_name_[static_cast<size_t>(index)] = name;
  }

  return ColIndex {index};
}

ColIndex LinearInterface::add_col(const std::string& name)
{
  const double collb = 0;
  const double colub = COIN_DBL_MAX;
  return add_col(name, collb, colub);
}

ColIndex LinearInterface::add_free_col(const std::string& name)
{
  const double collb = -COIN_DBL_MAX;
  const double colub = COIN_DBL_MAX;
  return add_col(name, collb, colub);
}

RowIndex LinearInterface::add_row(const std::string& name,
                                  const size_t numberElements,
                                  const std::span<const int>& columns,
                                  const std::span<const double>& elements,
                                  const double rowlb,
                                  const double rowub)
{
  const auto index = solver->getNumRows();
  check_name_unique(m_row_names_, name, index, "row", m_lp_names_level_);

  solver->addRow(static_cast<int>(numberElements),
                 columns.data(),
                 elements.data(),
                 rowlb,
                 rowub);
  solver->setRowName(index, name);

  return RowIndex {index};
}

RowIndex LinearInterface::add_row(const SparseRow& row, const double eps)
{
  const auto [columns, elements] = row.to_flat<int>(eps);

  return add_row(
      row.name, columns.size(), columns, elements, row.lowb, row.uppb);
}

void LinearInterface::set_coeff(const RowIndex row,
                                const ColIndex column,
                                const double value)
{
  const auto r = static_cast<int>(row);
  const auto c = static_cast<int>(column);

#ifdef COIN_USE_CLP
  // OsiClpSolverInterface provides modifyCoefficient() directly
  solver->modifyCoefficient(r, c, value, false);
#elifdef COIN_USE_CBC
  // OsiCbcSolverInterface wraps a CLP solver underneath
  auto* real_solver = solver->getRealSolverPtr();
  auto* clp_solver = dynamic_cast<OsiClpSolverInterface*>(real_solver);
  if (clp_solver != nullptr) {
    clp_solver->modifyCoefficient(r, c, value, false);
  }
  // If the underlying solver is not OsiClpSolverInterface the coefficient
  // modification is silently skipped; callers bear responsibility for
  // checking supports_set_coeff() before calling this function.
#elifdef COIN_USE_CPX
  // OsiCpxSolverInterface provides setCoefficient() which wraps CPXchgcoef
  solver->setCoefficient(r, c, value);
#else
  // Not implemented for this solver backend — silently skip.
  // Callers should check supports_set_coeff() first.
  (void)r;
  (void)c;
  (void)value;
#endif
}

double LinearInterface::get_coeff(const RowIndex row,
                                  const ColIndex column) const
{
  const auto* matrix = solver->getMatrixByCol();
  if (matrix == nullptr) {
    return 0.0;
  }
  return matrix->getCoefficient(static_cast<int>(row),
                                static_cast<int>(column));
}

bool LinearInterface::supports_set_coeff() noexcept
{
#if defined(COIN_USE_CLP) || defined(COIN_USE_CBC) || defined(COIN_USE_CPX)
  return true;
#else
  return false;
#endif
}

void LinearInterface::set_obj_coeff(const ColIndex index, const double value)
{
  solver->setObjCoeff(static_cast<int>(index), value);
}

void LinearInterface::set_col_low(const ColIndex index, const double value)
{
  solver->setColLower(static_cast<int>(index), value);
}

void LinearInterface::set_col_upp(const ColIndex index, const double value)
{
  solver->setColUpper(static_cast<int>(index), value);
}

void LinearInterface::set_row_low(const RowIndex index, const double value)
{
  solver->setRowLower(static_cast<int>(index), value);
}

void LinearInterface::set_row_upp(const RowIndex index, const double value)
{
  solver->setRowUpper(static_cast<int>(index), value);
}

void LinearInterface::set_col(const ColIndex index, const double value)
{
  set_col_low(index, value);
  set_col_upp(index, value);
}

void LinearInterface::set_rhs(const RowIndex row, const double rhs)
{
  solver->setRowBounds(static_cast<int>(row), rhs, rhs);
}

size_t LinearInterface::get_numrows() const
{
  return static_cast<size_t>(solver->getNumRows());
}

size_t LinearInterface::get_numcols() const
{
  return static_cast<size_t>(solver->getNumCols());
}

void LinearInterface::set_continuous(const ColIndex index)
{
  solver->setContinuous(static_cast<int>(index));
}

void LinearInterface::set_integer(const ColIndex index)
{
  solver->setInteger(static_cast<int>(index));
}

void LinearInterface::set_binary(const ColIndex index)
{
  set_integer(index);
  set_col_low(index, 0);
  set_col_upp(index, 1);
}

bool LinearInterface::is_continuous(const ColIndex index) const
{
  return solver->isContinuous(static_cast<int>(index));
}

bool LinearInterface::is_integer(const ColIndex index) const
{
  return solver->isInteger(static_cast<int>(index));
}

void LinearInterface::write_lp(const std::string& filename) const
{
  if (filename.empty()) {
    return;
  }
  solver->setIntParam(OsiNameDiscipline, 2);
  solver->writeLp(filename.c_str());
}

void LinearInterface::set_solver_opts(const SolverOptions& solver_options)
{
  if (const auto oeps = solver_options.optimal_eps; oeps && *oeps > 0) {
    solver->setDblParam(OsiDualTolerance, *oeps);
  }

  if (const auto feps = solver_options.feasible_eps; feps && *feps > 0) {
    solver->setDblParam(OsiPrimalTolerance, *feps);
  }

#ifdef OSI_EXTENDED
  if (const auto beps = solver_options.barrier_eps; beps && *beps > 0) {
    solver->setDblParam(OsiBarrierTolerance, *beps);
  }
#endif

  // Apply time limit if specified (must be set before algorithm hints)
  if (const auto tl = solver_options.time_limit; tl && *tl > 0.0) {
    set_time_limit(*tl);
  }

  // ── Warm-start override ──
  // When warm_start is true the LP is a clone that already carries a basis
  // (or crossover basis from barrier).  Force dual simplex and disable
  // presolve so the solver pivots from the existing basis rather than
  // restarting.  This is the primary optimisation for aperture/elastic
  // resolves when the original solve used barrier.
  if (solver_options.warm_start) {
    solver->setHintParam(OsiDoPresolveInInitial, false, OsiHintDo);
    solver->setHintParam(OsiDoPresolveInResolve, false, OsiHintDo);
    solver->setHintParam(OsiDoDualInInitial, true, OsiHintDo);
    solver->setHintParam(OsiDoDualInResolve, true, OsiHintDo);

    // Ensure barrier is disabled so the solver uses dual simplex.
    // The OsiDoBarrier hints require OSI_EXTENDED; without it, call the
    // underlying solver directly to force dual simplex.
#ifdef OSI_EXTENDED
    solver->setHintParam(OsiDoBarrierInInitial, false, OsiHintIgnore);
    solver->setHintParam(OsiDoBarrierInResolve, false, OsiHintIgnore);
#elifdef COIN_USE_CLP
    // ClpSolve algorithm 1 = dual simplex (avoids barrier)
    auto* clp_model = solver->getModelPtr();
    if (clp_model != nullptr) {
      clp_model->setAlgorithm(1);  // 1 = dual simplex
      // Bit 1: keep factorization (avoid re-factorising the basis)
      // Bit 8: keep work areas (avoid re-allocating internal arrays)
      constexpr int keep_factorization = 1;
      constexpr int keep_work_areas = 8;
      clp_model->setSpecialOptions(clp_model->specialOptions()
                                   | keep_factorization | keep_work_areas);
    }
#endif

    return;
  }

  const auto presolve = solver_options.presolve;
  solver->setHintParam(OsiDoPresolveInInitial, presolve, OsiHintDo);

  const bool On = true;
  const bool Off = false;
  const auto lp_algo = solver_options.algorithm;

  switch (lp_algo) {
    case LPAlgo::default_algo:
      break;
    case LPAlgo::primal: {
      solver->setHintParam(OsiDoDualInInitial, Off, OsiHintDo);
      solver->setHintParam(OsiDoDualInResolve, Off, OsiHintDo);

#ifdef OSI_EXTENDED
      solver->setHintParam(OsiDoBarrierInInitial, Off, OsiHintIgnore);
      solver->setHintParam(OsiDoBarrierInResolve, Off, OsiHintIgnore);
#endif
      break;
    }
    case LPAlgo::dual: {
      solver->setHintParam(OsiDoDualInInitial, On, OsiHintDo);
      solver->setHintParam(OsiDoDualInResolve, On, OsiHintDo);

#ifdef OSI_EXTENDED
      solver->setHintParam(OsiDoBarrierInInitial, Off, OsiHintIgnore);
      solver->setHintParam(OsiDoBarrierInResolve, Off, OsiHintIgnore);
#endif
      break;
    }
    case LPAlgo::barrier: {
#ifdef OSI_EXTENDED
      const auto threads = solver_options.threads;
      if (threads > 0) {
        solver->setIntParam(OsiNumThreads, threads);
      }
      solver->setHintParam(OsiDoDualInInitial, Off, OsiHintDo);
      solver->setHintParam(OsiDoBarrierInInitial, On, OsiHintDo);

      solver->setHintParam(OsiDoDualInResolve, Off, OsiHintDo);
      solver->setHintParam(OsiDoBarrierInResolve, On, OsiHintDo);
#endif
      break;
    }
    case LPAlgo::last_algo:
      break;
  }
}

std::expected<int, Error> LinearInterface::initial_solve(
    const SolverOptions& solver_options)
{
  try {
    set_solver_opts(solver_options);

    {
      const HandlerGuard guard(*this, solver_options.log_level);
      solver->initialSolve();
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
    set_solver_opts(solver_options);

    {
      const HandlerGuard guard(*this, solver_options.log_level);
      solver->resolve();
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

int LinearInterface::get_status() const
{
  try {
    if (solver->isProvenOptimal()) {
      return 0;
    }

    if (solver->isAbandoned()) {
      return 1;
    }

    if (solver->isProvenDualInfeasible() || solver->isProvenPrimalInfeasible())
    {
      return 2;
    }

    return 3;

  } catch (...) {
    return 1;
  }
}

double LinearInterface::get_kappa() const  // NOLINT
{
#ifdef OSI_EXTENDED
  try {
    return solver->getConditionNumber();
  } catch (...) {
    return 1;
  }
#else
  return 1;
#endif
}

bool LinearInterface::is_optimal() const
{
  return solver->isProvenOptimal();
}

bool LinearInterface::is_dual_infeasible() const
{
  return solver->isProvenDualInfeasible();
}

bool LinearInterface::is_prim_infeasible() const
{
  return solver->isProvenPrimalInfeasible();
}

double LinearInterface::get_obj_value() const
{
  return solver->getObjValue();
}

void LinearInterface::set_col_sol(const std::span<const double> sol)
{
  if (sol.data() != nullptr) {
    solver->setColSolution(sol.data());
  }
}

void LinearInterface::set_row_dual(const std::span<const double> dual)
{
  if (dual.data() != nullptr) {
    solver->setRowPrice(dual.data());
  }
}

}  // namespace gtopt
