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
  // Name tracking is handled by our own maps (m_col_names_, etc.).
  // OsiNameDiscipline = 0 avoids the solver's expensive per-element
  // name index.  Names are pushed to the solver only in write_lp().
  solver->setIntParam(OsiNameDiscipline, 0);
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
    if (col_sol.size() >= ncols) {
      // Pre-padded or exact-size vector: use a subspan (no allocation).
      set_col_sol(col_sol.first(ncols));
    } else {
      // Saved solution has fewer columns (e.g. elastic slacks added later).
      std::vector<double> padded(ncols, 0.0);
      std::ranges::copy(col_sol, padded.begin());
      set_col_sol(padded);
    }
  }
  if (!row_dual.empty()) {
    const auto nrows = static_cast<std::size_t>(get_numrows());
    if (row_dual.size() >= nrows) {
      // Pre-padded or exact-size vector: use a subspan (no allocation).
      set_row_dual(row_dual.first(nrows));
    } else {
      // Saved dual has fewer rows (e.g. Benders cuts added later).
      std::vector<double> padded(nrows, 0.0);
      std::ranges::copy(row_dual, padded.begin());
      set_row_dual(padded);
    }
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

  // Build name maps: populate the unordered_map with O(1) amortized inserts.
  auto build_name_map = [](const auto& names_vec,
                           name_index_map_t& name_map,
                           std::vector<std::string>& index_to_name)
  {
    index_to_name.resize(names_vec.size());
    name_map.reserve(names_vec.size());

    for (int i = 0; const auto& name : names_vec) {
      if (!name.empty()) {
        index_to_name[static_cast<size_t>(i)] = name;
        name_map.emplace(name, i);  // first occurrence wins
      }
      ++i;
    }
  };

  if (m_lp_names_level_ >= 1 && !flat_lp.colnm.empty()) {
    build_name_map(flat_lp.colnm, m_col_names_, m_col_index_to_name_);
  }

  // Row name→index map is only needed at level >= 2 (duplicate error
  // detection).  At level 1 we still populate the index→name vector
  // (used by sddp_cut_io and cascade_solver via col_index_to_name).
  if (m_lp_names_level_ >= 2 && !flat_lp.rownm.empty()) {
    build_name_map(flat_lp.rownm, m_row_names_, m_row_index_to_name_);
  } else if (m_lp_names_level_ >= 1 && !flat_lp.rownm.empty()) {
    m_row_index_to_name_.resize(flat_lp.rownm.size());
    for (int i = 0; const auto& name : flat_lp.rownm) {
      if (!name.empty()) {
        m_row_index_to_name_[static_cast<size_t>(i)] = name;
      }
      ++i;
    }
  }

  // At level >= 2 push names to the solver for nice LP output.
  // Use ClpModel::copyNames() for O(1) bulk copy when available,
  // falling back to per-element setColName/setRowName otherwise.
  if (m_lp_names_level_ >= 2) {
    push_names_to_solver();
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

  // At level >= 2 pass the name so the solver can produce nice LP files.
  // At lower levels pass empty (OsiNameDiscipline = 0, no overhead).
  solver->addCol(
      vec, collb, colub, obj, m_lp_names_level_ >= 2 ? name : std::string {});

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
  // Row name→index map only at level >= 2 (duplicate error detection).
  if (m_lp_names_level_ >= 2) {
    check_name_unique(m_row_names_, name, index, "row", m_lp_names_level_);
  }

  solver->addRow(static_cast<int>(numberElements),
                 columns.data(),
                 elements.data(),
                 rowlb,
                 rowub);
  // No solver->setRowName() — OsiNameDiscipline = 0.

  if (m_lp_names_level_ >= 1 && !name.empty()) {
    if (m_row_index_to_name_.size() <= static_cast<size_t>(index)) {
      m_row_index_to_name_.resize(static_cast<size_t>(index) + 1);
    }
    m_row_index_to_name_[static_cast<size_t>(index)] = name;
  }

  return RowIndex {index};
}

RowIndex LinearInterface::add_row(const SparseRow& row, const double eps)
{
  const auto [columns, elements] = row.to_flat<int>(eps);

  return add_row(
      row.name, columns.size(), columns, elements, row.lowb, row.uppb);
}

void LinearInterface::delete_rows(const std::span<const int> indices)
{
  if (indices.empty()) {
    return;
  }

  solver->deleteRows(static_cast<int>(indices.size()), indices.data());

  // Update row name maps: erase deleted indices from the vector (reverse
  // order keeps remaining indices valid), then rebuild the hash map.
  if (m_lp_names_level_ >= 1) {
    for (const auto idx : indices | std::views::reverse) {
      const auto pos = static_cast<ptrdiff_t>(idx);
      if (pos < std::ssize(m_row_index_to_name_)) {
        m_row_index_to_name_.erase(m_row_index_to_name_.begin() + pos);
      }
    }
    rebuild_row_name_maps();
  }
}

void LinearInterface::rebuild_row_name_maps()
{
  // Rebuild the name→index hash map from the index→name vector.
  // Only at level >= 2 (row_name_map is unused in production).
  if (m_lp_names_level_ >= 2) {
    m_row_names_.clear();
    m_row_names_.reserve(m_row_index_to_name_.size());
    for (int32_t i = 0; const auto& name : m_row_index_to_name_) {
      if (!name.empty()) {
        m_row_names_.emplace(name, i);
      }
      ++i;
    }
  }
}

void LinearInterface::reset_from(const LinearInterface& source,
                                 const size_t base_rows)
{
  // 1. Delete any rows beyond the base (Benders cuts added since the
  //    clone was created or last reset).
  const auto total_rows = static_cast<size_t>(solver->getNumRows());
  if (total_rows > base_rows) {
    const auto n_to_delete = total_rows - base_rows;
    std::vector<int> indices;
    indices.reserve(n_to_delete);
    for (auto i = base_rows; i < total_rows; ++i) {
      indices.push_back(static_cast<int>(i));
    }
    solver->deleteRows(static_cast<int>(n_to_delete), indices.data());
  }

  // 2. Copy column bounds from the source LP.
  const auto ncols = solver->getNumCols();
  const auto src_col_lo = source.get_col_low();
  const auto src_col_hi = source.get_col_upp();
  for (int c = 0; c < ncols; ++c) {
    solver->setColLower(c, src_col_lo[c]);
    solver->setColUpper(c, src_col_hi[c]);
  }

  // 3. Copy row bounds from the source LP (structural rows only).
  const auto nrows = solver->getNumRows();
  const auto src_row_lo = source.get_row_low();
  const auto src_row_hi = source.get_row_upp();
  for (int r = 0; r < nrows; ++r) {
    solver->setRowLower(r, src_row_lo[r]);
    solver->setRowUpper(r, src_row_hi[r]);
  }

  // Truncate row name vector to base size and rebuild hash map.
  if (m_lp_names_level_ >= 1) {
    m_row_index_to_name_.resize(static_cast<size_t>(nrows));
    rebuild_row_name_maps();
  }
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
#elifdef COIN_USE_HGS
  // OsiHiGHS: use the generic OSI modifyCoefficient path
  solver->modifyCoefficient(r, c, value, false);
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
#if defined(COIN_USE_CLP) || defined(COIN_USE_CBC) || defined(COIN_USE_CPX) \
    || defined(COIN_USE_HGS)
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

void LinearInterface::push_names_to_solver() const
{
  const auto ncols = static_cast<size_t>(solver->getNumCols());
  const auto nrows = static_cast<size_t>(solver->getNumRows());

  // Pad vectors to match solver dimensions (our vectors may be shorter
  // if elastic/cut columns/rows were added without names).
  std::vector<std::string> col_names(ncols);
  for (size_t i = 0; i < std::min(m_col_index_to_name_.size(), ncols); ++i) {
    col_names[i] = m_col_index_to_name_[i];
  }
  std::vector<std::string> row_names(nrows);
  for (size_t i = 0; i < std::min(m_row_index_to_name_.size(), nrows); ++i) {
    row_names[i] = m_row_index_to_name_[i];
  }

#ifdef COIN_USE_CLP
  // Fast path: ClpModel::copyNames() sets both name vectors and
  // lengthNames_ in one pass — no per-element overhead.
  solver->getModelPtr()->copyNames(row_names, col_names);
#elifdef COIN_USE_CBC
  auto* real_solver = solver->getRealSolverPtr();
  auto* clp_solver = dynamic_cast<OsiClpSolverInterface*>(real_solver);
  if (clp_solver != nullptr) {
    clp_solver->getModelPtr()->copyNames(row_names, col_names);
  }
#elifdef COIN_USE_CPX
  // Fast path: bulk name setting via CPLEX C API (CPXchgcolname/CPXchgrowname)
  // avoids per-element OSI overhead.
  {
    auto* env = solver->getEnvironmentPtr();
    auto* lp = solver->getLpPtr(OsiCpxSolverInterface::KEEPCACHED_ALL);

    // Build index + c-string arrays for CPXchgcolname
    std::vector<int> col_indices;
    std::vector<char*> col_cnames;
    col_indices.reserve(ncols);
    col_cnames.reserve(ncols);
    for (size_t i = 0; i < ncols; ++i) {
      if (!col_names[i].empty()) {
        col_indices.push_back(static_cast<int>(i));
        col_cnames.push_back(col_names[i].data());
      }
    }
    if (!col_indices.empty()) {
      CPXchgcolname(env,
                    lp,
                    static_cast<int>(col_indices.size()),
                    col_indices.data(),
                    col_cnames.data());
    }

    // Build index + c-string arrays for CPXchgrowname
    std::vector<int> row_indices;
    std::vector<char*> row_cnames;
    row_indices.reserve(nrows);
    row_cnames.reserve(nrows);
    for (size_t i = 0; i < nrows; ++i) {
      if (!row_names[i].empty()) {
        row_indices.push_back(static_cast<int>(i));
        row_cnames.push_back(row_names[i].data());
      }
    }
    if (!row_indices.empty()) {
      CPXchgrowname(env,
                    lp,
                    static_cast<int>(row_indices.size()),
                    row_indices.data(),
                    row_cnames.data());
    }
  }
#else
  // Generic fallback: per-element via OSI (requires discipline >= 1).
  // Works for HiGHS and any other OSI-compatible solver.
  solver->setIntParam(OsiNameDiscipline, 2);
  for (size_t i = 0; i < ncols; ++i) {
    if (!col_names[i].empty()) {
      solver->setColName(static_cast<int>(i), col_names[i]);
    }
  }
  for (size_t i = 0; i < nrows; ++i) {
    if (!row_names[i].empty()) {
      solver->setRowName(static_cast<int>(i), row_names[i]);
    }
  }
#endif
}

void LinearInterface::write_lp(const std::string& filename) const
{
  if (filename.empty()) {
    return;
  }
  // At level >= 2 names are already on the solver (set during load_flat
  // and kept in sync by add_col/add_row).  At level < 2 push them now.
  if (m_lp_names_level_ < 2) {
    push_names_to_solver();
  }
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
      constexpr unsigned keep_factorization = 1U;
      constexpr unsigned keep_work_areas = 8U;
      clp_model->setSpecialOptions(static_cast<int>(
          clp_model->specialOptions() | keep_factorization | keep_work_areas));
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
