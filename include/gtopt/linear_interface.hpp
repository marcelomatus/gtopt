/**
 * @file      linear_interface.hpp
 * @brief     Interface to linear programming solvers
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a unified interface to various linear programming
 * solvers through a pluggable SolverBackend abstraction.  It enables
 * problem construction, solving, and solution retrieval in a solver-agnostic
 * manner, simplifying the integration of different planning engines.
 *
 * Solver backends (CLP, CBC, CPLEX, HiGHS, …) are loaded as dynamic
 * plugins at runtime via the SolverRegistry.
 */

#pragma once

#include <expected>
#include <memory>
#include <span>
#include <unordered_map>

#include <gtopt/error.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class LinearInterface
{
public:
  /** @brief Copy constructor disabled */
  explicit LinearInterface(const LinearInterface&) = delete;
  /** @brief Copy assignment disabled */
  LinearInterface& operator=(const LinearInterface&) = delete;
  /** @brief Move constructor */
  LinearInterface(LinearInterface&&) = default;
  /** @brief Move assignment */
  LinearInterface& operator=(LinearInterface&&) = default;

  /**
   * @brief Constructs interface with a solver backend by name
   * @param solver_name Solver identifier ("clp", "cbc", "cplex", "highs")
   * @param plog_file Path to log file for solver output
   */
  explicit LinearInterface(std::string_view solver_name = {},
                           const std::string& plog_file = {});

  /**
   * @brief Constructs interface with a pre-created backend
   * @param backend Pre-configured solver backend
   * @param plog_file Path to log file for solver output
   */
  explicit LinearInterface(std::unique_ptr<SolverBackend> backend,
                           const std::string& plog_file = {});

  /**
   * @brief Constructs interface, loads a problem, with solver by name
   * @param solver_name Solver identifier
   * @param flat_lp Flattened linear problem to load
   * @param plog_file Path to log file for solver output
   */
  LinearInterface(std::string_view solver_name,
                  const FlatLinearProblem& flat_lp,
                  const std::string& plog_file = {});

  ~LinearInterface() = default;

  /**
   * @brief Creates a deep copy of this LinearInterface via the backend's
   *        clone() method.
   *
   * The clone preserves the full LP state (variables, constraints, bounds,
   * objective, warm-start basis).  Modifications to the clone do not affect
   * the original — use this for tentative solves such as the SDDP elastic
   * filter, where the original LP must remain unmodified.
   *
   * @return A new LinearInterface wrapping the cloned solver.
   */
  [[nodiscard]] LinearInterface clone() const;

  /**
   * @brief Apply a saved solution as warm-start hint for the next resolve.
   *
   * Handles dimension mismatches gracefully: if the LP has gained extra
   * columns (e.g. elastic slack variables) or extra rows (e.g. Benders cuts)
   * since the solution was captured, the vectors are zero-padded to match.
   * If the saved vector is larger than the current LP dimension it is
   * silently ignored (stale snapshot).
   *
   * @param col_sol  Primal solution to set (empty = skip)
   * @param row_dual Dual solution to set (empty = skip)
   */
  void set_warm_start_solution(std::span<const double> col_sol,
                               std::span<const double> row_dual);

  /**
   * @brief Loads a flattened linear problem into the solver
   * @param flat_lp The flattened problem representation
   * @throws std::runtime_error if the problem cannot be loaded
   */
  void load_flat(const FlatLinearProblem& flat_lp);

  /**
   * @brief Adds a new column (variable) to the problem
   * @param name The name of the column
   * @return The index of the newly added column
   */
  ColIndex add_col(const std::string& name);

  /**
   * @brief Adds a new column (variable) with bounds to the problem
   * @param name The name of the column
   * @param collb Lower bound for the column
   * @param colub Upper bound for the column
   * @return The index of the newly added column
   */
  ColIndex add_col(const std::string& name, double collb, double colub);

  /**
   * @brief Adds a new unbounded column (free variable) to the problem
   * @param name The name of the column
   * @return The index of the newly added column
   */
  ColIndex add_free_col(const std::string& name);

  /**
   * @brief Adds a new constraint row to the problem
   * @param row The sparse row representation of the constraint
   * @param eps Epsilon value for coefficient filtering (values below this are
   * ignored)
   * @return The index of the newly added row
   */
  RowIndex add_row(const SparseRow& row, double eps = 0.0);

  /**
   * @brief Deletes rows by index from the constraint matrix.
   *
   * @param indices  Row indices to delete (must be sorted ascending)
   */
  void delete_rows(std::span<const int> indices);

  /**
   * @brief Save the current row count as the "base" model size.
   *
   * All rows added after this point are considered cuts and are eligible
   * for pruning.  Must be called once after the structural LP is built,
   * before any Benders cuts are added.
   */
  void save_base_numrows() noexcept { m_base_numrows_ = get_numrows(); }

  /**
   * @brief Get the saved base row count.
   * @return Base row count (0 if save_base_numrows was never called)
   */
  [[nodiscard]] constexpr size_t base_numrows() const noexcept
  {
    return m_base_numrows_;
  }

  /**
   * @brief Reset this LP to a base state by copying column bounds from
   *        @p source and deleting any rows beyond @p base_rows.
   *
   * Used by the clone pool to reuse a cached LP clone across aperture
   * solves without re-allocating the underlying solver.
   *
   * @param source    The original (unmodified) LP to copy bounds from
   * @param base_rows Number of structural rows to keep (delete beyond)
   */
  void reset_from(const LinearInterface& source, size_t base_rows);

  /**
   * @brief Gets the number of constraint rows in the problem
   * @return Number of rows
   */
  [[nodiscard]] size_t get_numrows() const;

  /**
   * @brief Gets the number of variable columns in the problem
   * @return Number of columns
   */
  [[nodiscard]] size_t get_numcols() const;

  /// Solver backend name (e.g. "clp", "cplex", "highs").
  [[nodiscard]] std::string_view solver_name() const noexcept
  {
    return m_backend_->solver_name();
  }

  /// Solver library version string (e.g. "1.17.3").
  [[nodiscard]] std::string solver_version() const
  {
    return m_backend_->solver_version();
  }

  /// Solver identifier: "name/version" (e.g. "highs/1.13.1").
  [[nodiscard]] std::string solver_id() const
  {
    return std::format("{}/{}", solver_name(), solver_version());
  }

  /// Currently configured LP algorithm.
  [[nodiscard]] LPAlgo get_algorithm() const
  {
    return m_backend_->get_algorithm();
  }

  /// Currently configured thread count (0 = solver default).
  [[nodiscard]] int get_threads() const { return m_backend_->get_threads(); }

  /// Whether presolve is currently enabled.
  [[nodiscard]] bool get_presolve() const { return m_backend_->get_presolve(); }

  /// Current solver log verbosity level (0 = off).
  [[nodiscard]] int get_log_level() const
  {
    return m_backend_->get_log_level();
  }

  /// Solver's representation of +infinity for variable bounds.
  [[nodiscard]] double infinity() const noexcept
  {
    return m_backend_->infinity();
  }

  /// Normalize a bound value: map gtopt::DblMax to the solver's infinity.
  ///
  /// SparseCol/SparseRow use `std::numeric_limits<double>::max()` as default
  /// unbounded markers, but solver backends (e.g. HiGHS) may use a smaller
  /// infinity threshold (e.g. 1e30).  This method translates at the
  /// LinearInterface boundary so that formulation code and solver agree.
  [[nodiscard]] double normalize_bound(double value) const noexcept
  {
    const auto inf = m_backend_->infinity();
    if (value >= DblMax) {
      return inf;
    }
    if (value <= -DblMax) {
      return -inf;
    }
    return value;
  }

  /// True if @p value represents positive infinity for the active solver.
  [[nodiscard]] bool is_pos_inf(double value) const noexcept
  {
    return value >= m_backend_->infinity();
  }

  /// True if @p value represents negative infinity for the active solver.
  [[nodiscard]] bool is_neg_inf(double value) const noexcept
  {
    return value <= -m_backend_->infinity();
  }

  void set_rhs(RowIndex row, double rhs);
  void set_row_low(RowIndex index, double value);
  void set_row_upp(RowIndex index, double value);

  /**
   * @brief Gets a coefficient value from the constraint matrix
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @return The coefficient value, or 0.0 if the element does not exist
   */
  [[nodiscard]] double get_coeff(RowIndex row, ColIndex column) const;

  /**
   * @brief Sets (modifies) a coefficient in the constraint matrix
   *
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @param value New coefficient value
   */
  void set_coeff(RowIndex row, ColIndex column, double value);

  /**
   * @brief Checks whether the solver supports in-place coefficient updates
   *
   * @return true if set_coeff() is functional
   */
  [[nodiscard]] bool supports_set_coeff() const noexcept;

  void set_obj_coeff(ColIndex index, double value);

  [[nodiscard]] auto get_obj_coeff() const
  {
    return std::span(m_backend_->obj_coefficients(), get_numcols());
  }

  void set_col_low(ColIndex index, double value);
  void set_col_upp(ColIndex index, double value);
  void set_col(ColIndex index, double value);

  [[nodiscard]] double get_obj_value() const;

  /**
   * @brief Writes the problem to an LP format file
   * @param filename Name of the file to write (without extension)
   * @return Success, or an error if row names are not available
   */
  [[nodiscard]] std::expected<void, Error> write_lp(
      const std::string& filename) const;

  /**
   * @brief Performs initial solve of the problem from scratch
   * @param solver_options Options controlling the solve process
   * @return Expected with solver status code (0 = optimal) or error
   */
  [[nodiscard]] std::expected<int, Error> initial_solve(
      const SolverOptions& solver_options = {});

  /**
   * @brief Resolves the problem with updated data using warm start
   * @param solver_options Options controlling the solve process
   * @return Expected with solver status code (0 = optimal) or error
   */
  [[nodiscard]] std::expected<int, Error> resolve(
      const SolverOptions& solver_options = {});

  /**
   * @brief Gets the condition number of the basis matrix (if available)
   * @return Condition number kappa, or 1.0 if not available
   */
  [[nodiscard]] double get_kappa() const;

  /**
   * @brief Gets the solver-specific status code
   * @return Status code (interpretation depends on solver)
   */
  [[nodiscard]] int get_status() const;

  /**
   * @brief Checks if the solution is optimal
   * @return True if optimal solution found, false otherwise
   */
  [[nodiscard]] bool is_optimal() const;

  /**
   * @brief Checks if the problem is dual infeasible
   * @return True if dual infeasible, false otherwise
   */
  [[nodiscard]] bool is_dual_infeasible() const;

  /**
   * @brief Checks if the problem is primal infeasible
   * @return True if primal infeasible, false otherwise
   */
  [[nodiscard]] bool is_prim_infeasible() const;

  /**
   * @brief Sets a variable to be continuous (floating-point)
   * @param index Column index to modify
   */
  void set_continuous(ColIndex index);

  /**
   * @brief Sets a variable to be integer
   * @param index Column index to modify
   */
  void set_integer(ColIndex index);

  /**
   * @brief Sets a variable to be binary (0-1 integer)
   * @param index Column index to modify
   */
  void set_binary(ColIndex index);

  /**
   * @brief Checks if a variable is continuous
   * @param index Column index to check
   * @return True if continuous, false otherwise
   */
  [[nodiscard]] bool is_continuous(ColIndex index) const;

  /**
   * @brief Checks if a variable is integer
   * @param index Column index to check
   * @return True if integer, false otherwise
   */
  [[nodiscard]] bool is_integer(ColIndex index) const;

  /**
   * @brief Sets a time limit for the solver
   * @param time_limit Maximum solve time in seconds
   */
  void set_time_limit(double time_limit);

  /**
   * @brief Gets the lower bounds for all constraint rows
   * @return Span view of row lower bounds
   */
  [[nodiscard]] auto get_row_low() const
  {
    return std::span(m_backend_->row_lower(), get_numrows());
  }

  /**
   * @brief Gets the upper bounds for all constraint rows
   * @return Span view of row upper bounds
   */
  [[nodiscard]] auto get_row_upp() const
  {
    return std::span(m_backend_->row_upper(), get_numrows());
  }

  /**
   * @brief Gets the lower bounds for all variable columns
   * @return Span view of column lower bounds
   */
  [[nodiscard]] auto get_col_low() const
  {
    return std::span(m_backend_->col_lower(), get_numcols());
  }

  /**
   * @brief Gets the upper bounds for all variable columns
   * @return Span view of column upper bounds
   */
  [[nodiscard]] auto get_col_upp() const
  {
    return std::span(m_backend_->col_upper(), get_numcols());
  }

  /**
   * @brief Gets the solution values for all variables
   * @return Span view of solution values
   */
  [[nodiscard]] auto get_col_sol() const
  {
    return std::span(m_backend_->col_solution(), get_numcols());
  }

  /**
   * @brief Gets the reduced costs for all variables
   * @return Span view of reduced costs
   */
  [[nodiscard]] auto get_col_cost() const
  {
    return std::span(m_backend_->reduced_cost(), get_numcols());
  }

  /**
   * @brief Gets the physical-to-LP scale factor for a single column.
   *
   * physical_value = LP_value × scale.  A scale of 1.0 means no scaling
   * (LP variable == physical value).  Scale factors are populated during
   * load_flat() from FlatLinearProblem::col_scales.
   *
   * @param index Column index
   * @return Scale factor (1.0 if col_scales is empty or not populated)
   */
  [[nodiscard]] constexpr double get_col_scale(ColIndex index) const noexcept
  {
    const auto i = static_cast<size_t>(index);
    if (i < m_col_scales_.size()) {
      return m_col_scales_[i];
    }
    return 1.0;
  }

  /**
   * @brief Gets all column scale factors.
   * @return Const reference to the column scale vector (empty if not
   * populated)
   */
  [[nodiscard]] constexpr const std::vector<double>& get_col_scales()
      const noexcept
  {
    return m_col_scales_;
  }

  /**
   * @brief Gets the dual values (shadow prices) for all constraints
   * @return Span view of dual values
   */
  [[nodiscard]] auto get_row_dual() const
  {
    return std::span(m_backend_->row_price(), get_numrows());
  }

  void set_col_sol(std::span<const double> sol);
  void set_row_dual(std::span<const double> dual);

  void set_log_file(const std::string& plog_file);
  [[nodiscard]] constexpr const auto& get_log_file() const
  {
    return m_log_file_;
  }

  void set_prob_name(const std::string& pname);
  [[nodiscard]] std::string get_prob_name() const;

  /**
   * @brief Sets the LP name uniqueness-check level.
   *
   * Controls name tracking and duplicate detection for add_row()/add_col():
   *   - 0: col names tracked, row names disabled
   *   - 1: col + row names tracked, warn on duplicate names via spdlog
   *   - 2: col + row names tracked + throw std::runtime_error on duplicates
   *
   * Col name-to-index maps are populated at level >= 0.
   * Row name-to-index maps are populated at level >= 1.
   * The overhead is a single map insert per add_row/add_col call.
   *
   * @param level The LP name check level (matches names_level semantics)
   */
  void set_lp_names_level(int level) noexcept { m_lp_names_level_ = level; }

  /**
   * @brief Gets the current LP name uniqueness-check level.
   * @return 0-2 level (see set_lp_names_level)
   */
  [[nodiscard]] constexpr int lp_names_level() const noexcept
  {
    return m_lp_names_level_;
  }

  /// @name Name-to-index maps (col: level >= 0, row: level >= 1)
  /// @{

  /// Row (constraint) name → row index map.
  using name_index_map_t = std::unordered_map<std::string, int32_t>;

  [[nodiscard]] constexpr const name_index_map_t& row_name_map() const noexcept
  {
    return m_row_names_;
  }

  [[nodiscard]] constexpr const name_index_map_t& col_name_map() const noexcept
  {
    return m_col_names_;
  }

  /// Column index → name vector (empty string for unnamed columns).
  /// Populated alongside col_name_map when lp_names_level >= 1.
  [[nodiscard]] constexpr const std::vector<std::string>& col_index_to_name()
      const noexcept
  {
    return m_col_index_to_name_;
  }

  /// Row index → name vector (empty string for unnamed rows).
  /// Populated alongside row_name_map when lp_names_level >= 1.
  [[nodiscard]] constexpr const std::vector<std::string>& row_index_to_name()
      const noexcept
  {
    return m_row_index_to_name_;
  }
  /// @}

  /// @name Warm column solution (hot-start state)
  /// @{

  /// Return the warm column solution vector (empty if no state loaded).
  [[nodiscard]] constexpr const std::vector<double>& warm_col_sol()
      const noexcept
  {
    return m_warm_col_sol_;
  }

  /// Set the warm column solution from a loaded state file.
  void set_warm_col_sol(std::vector<double> sol) noexcept
  {
    m_warm_col_sol_ = std::move(sol);
  }
  /// @}

  /// @name LP coefficient statistics (populated during load_flat from
  ///       FlatLinearProblem::stats_* fields, which are computed in
  ///       LinearProblem::lp_build when LpBuildOptions::compute_stats is true).
  /// @{
  [[nodiscard]] constexpr size_t lp_stats_nnz() const noexcept
  {
    return m_stats_nnz_;
  }
  [[nodiscard]] constexpr size_t lp_stats_zeroed() const noexcept
  {
    return m_stats_zeroed_;
  }
  [[nodiscard]] constexpr double lp_stats_max_abs() const noexcept
  {
    return m_stats_max_abs_;
  }
  [[nodiscard]] constexpr double lp_stats_min_abs() const noexcept
  {
    return m_stats_min_abs_;
  }
  [[nodiscard]] constexpr FlatLinearProblem::index_t lp_stats_max_col()
      const noexcept
  {
    return m_stats_max_col_;
  }
  [[nodiscard]] constexpr FlatLinearProblem::index_t lp_stats_min_col()
      const noexcept
  {
    return m_stats_min_col_;
  }
  [[nodiscard]] constexpr const std::string& lp_stats_max_col_name()
      const noexcept
  {
    return m_stats_max_col_name_;
  }
  [[nodiscard]] constexpr const std::string& lp_stats_min_col_name()
      const noexcept
  {
    return m_stats_min_col_name_;
  }
  /// @}

private:
  RowIndex add_row(const std::string& name,
                   size_t numberElements,
                   const std::span<const int>& columns,
                   const std::span<const double>& elements,
                   double rowlb,
                   double rowub);

  void rebuild_row_name_maps();
  void push_names_to_solver() const;

  struct HandlerGuard
  {
    LinearInterface* interface;

    HandlerGuard(HandlerGuard const&) = delete;
    HandlerGuard& operator=(HandlerGuard const&) = delete;
    HandlerGuard(HandlerGuard&&) = delete;
    HandlerGuard& operator=(HandlerGuard&&) = delete;

    constexpr explicit HandlerGuard(LinearInterface& pinterface, int log_level)
        : interface(&pinterface)
    {
      interface->open_log_handler(log_level);
    }

    ~HandlerGuard() { interface->close_log_handler(); }
  };

  /// RAII guard for solver-native file-based logging (set_log_filename API).
  struct LogFileGuard
  {
    LinearInterface* interface;

    LogFileGuard(LogFileGuard const&) = delete;
    LogFileGuard& operator=(LogFileGuard const&) = delete;
    LogFileGuard(LogFileGuard&&) = delete;
    LogFileGuard& operator=(LogFileGuard&&) = delete;

    explicit LogFileGuard(LinearInterface& li,
                          const std::string& filename,
                          int level)
        : interface(&li)
    {
      interface->m_backend_->set_log_filename(filename, level);
    }

    ~LogFileGuard() { interface->m_backend_->clear_log_filename(); }
  };

  void open_log_handler(int log_level);
  void close_log_handler();

  std::unique_ptr<SolverBackend> m_backend_;
  std::string m_log_file_ {};
  int m_lp_names_level_ {};  ///< LP name uniqueness-check level (0–2)

  /// Name-to-index maps for duplicate detection and later lookup.
  /// Populated when lp_names_level >= 1.
  name_index_map_t m_row_names_;  ///< Row (constraint) name → row index
  name_index_map_t m_col_names_;  ///< Column (variable) name → col index
  std::vector<std::string> m_col_index_to_name_;  ///< Col index → name
  std::vector<std::string> m_row_index_to_name_;  ///< Row index → name

  size_t m_base_numrows_ {};  ///< Row count before any cuts were added

  std::vector<double> m_col_scales_;  ///< Per-column physical-to-LP scale
                                      ///< factors (physical = LP × scale)

  /// Warm column solution loaded from a previous run's state file.
  /// Used by StorageLP::physical_eini/efin as fallback when
  /// !is_optimal() (hot start before first solve).  Empty if no
  /// state was loaded.
  std::vector<double> m_warm_col_sol_;

  size_t m_stats_nnz_ {};
  size_t m_stats_zeroed_ {};
  double m_stats_max_abs_ {};
  double m_stats_min_abs_ {};
  FlatLinearProblem::index_t m_stats_max_col_ {-1};
  FlatLinearProblem::index_t m_stats_min_col_ {-1};
  std::string m_stats_max_col_name_ {};
  std::string m_stats_min_col_name_ {};

  struct FILEcloser
  {
    auto operator()(auto f) const { return fclose(f); }
  };
  using log_file_ptr_t = std::unique_ptr<FILE, FILEcloser>;
  log_file_ptr_t m_log_file_ptr_;
};

}  // namespace gtopt
