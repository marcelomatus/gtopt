#include <cerrno>
#include <format>
#include <memory>

#include <coin/CoinPackedVector.hpp>
#include <gtopt/linear_interface.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

void LinearInterface::set_prob_name(const std::string& pname)
{
  solver->setStrParam(OsiProbName, pname);
}

void LinearInterface::set_log_file(const std::string& plog_file)
{
  this->log_file = plog_file;
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
    auto file = log_file + ".log";
    log_file_ptr = log_file_ptr_t(std::fopen(file.c_str(), "ae"));

    if (!log_file_ptr) {
      const auto msg = std::format(
          "failed to open solver log file {} : errno", log_file, errno);

      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
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

  for (auto i : flat_lp.colint) {
    solver->setInteger(i);
  }

  for (int i = 0; auto&& name : flat_lp.colnm) {
    solver->setColName(i++, name);
  }

  for (int i = 0; auto&& name : flat_lp.rownm) {
    solver->setRowName(i++, name);
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

  const CoinPackedVector vec;
  const double obj = 0;

  solver->addCol(vec, collb, colub, obj, name);

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

#ifdef OSI_EXTENDED
void LinearInterface::set_coeff(const RowIndex row,
                                const ColIndex column,
                                const double value)
{
  solver->setCoefficient(
      static_cast<int>(row), static_cast<int>(column), value);
}

double LinearInterface::get_coeff(RowIndex row, size_t column) const
{
  return solver->getCoefficient(static_cast<int>(row),
                                static_cast<int>(column));
}
#endif

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

  solver->writeLp(filename.c_str());
}

void LinearInterface::set_solver_opts(const SolverOptions& solver_options)
{
  const auto oeps = solver_options.optimal_eps;
  if (oeps > 0) {
    solver->setDblParam(OsiDualTolerance, oeps);
  }

  const auto feps = solver_options.feasible_eps;
  if (feps > 0) {
    solver->setDblParam(OsiPrimalTolerance, feps);
  }

#ifdef OSI_EXTENDED
  const auto beps = solver_options.barrier_eps;
  if (beps > 0) {
    solver->setDblParam(OsiBarrierTolerance, beps);
  }
#endif

  const auto presolve = solver_options.presolve;
  solver->setHintParam(OsiDoPresolveInInitial, presolve, OsiHintDo);

  const bool On = true;
  const bool Off = false;
  const auto lp_algo = static_cast<LPAlgo>(solver_options.algorithm);

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

bool LinearInterface::initial_solve(const SolverOptions& solver_options)
{
  set_solver_opts(solver_options);

  const HandlerGuard guard(*this, solver_options.log_level);
  solver->initialSolve();

  return is_optimal();
}

bool LinearInterface::resolve(const SolverOptions& solver_options)
{
  set_solver_opts(solver_options);

  const HandlerGuard guard(*this, solver_options.log_level);
  solver->resolve();

  return is_optimal();
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
