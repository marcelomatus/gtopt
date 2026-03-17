/**
 * @file      linear_interface.hpp
 * @brief     Interface to linear programming solvers
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a unified interface to various linear programming
 * solvers through the OSI (Open Solver Interface) library. It enables
 * problem construction, solving, and solution retrieval in a solver-agnostic
 * manner, simplifying the integration of different planning engines.
 */

#pragma once

#include <expected>
#include <memory>

#include <gtopt/error.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/osi_solver.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

class LinearInterface
{
public:
  using SolverInterface = ::osiSolverInterface;
  using solver_ptr_t = std::shared_ptr<SolverInterface>;

  /** @brief Copy constructor disabled */
  explicit LinearInterface(const LinearInterface&) = delete;
  /** @brief Copy assignment disabled */
  LinearInterface& operator=(const LinearInterface&) = delete;
  /** @brief Move constructor disabled */
  LinearInterface(LinearInterface&&) = default;
  /** @brief Move assignment disabled */
  LinearInterface& operator=(LinearInterface&&) = default;

  /**
   * @brief Constructs interface with a default solver
   * @param plog_file Path to log file for solver output
   */
  explicit LinearInterface(const std::string& plog_file = {});

  /**
   * @brief Constructs interface with an existing solver
   * @param psolver Pre-configured solver pointer
   * @param plog_file Path to log file for solver output
   */
  LinearInterface(solver_ptr_t psolver, std::string plog_file);

  /**
   * @brief Constructs interface and loads a problem
   * @param flat_lp Flattened linear problem to load
   * @param plog_file Path to log file for solver output
   */
  explicit LinearInterface(const FlatLinearProblem& flat_lp,
                           const std::string& plog_file = {});

  ~LinearInterface() = default;

  /**
   * @brief Creates a deep copy of this LinearInterface via OsiSolverInterface
   *        clone().
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
   * @brief Gets the number of constraint rows in the problem
   * @return Number of rows
   */
  [[nodiscard]] size_t get_numrows() const;

  /**
   * @brief Gets the number of variable columns in the problem
   * @return Number of columns
   */
  [[nodiscard]] size_t get_numcols() const;

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
   * Uses solver-specific methods to update the matrix element in place:
   * - CLP:  `OsiClpSolverInterface::modifyCoefficient()`
   * - CBC:  delegates to the underlying CLP solver
   * - CPLEX: `CPXchgcoef()` via `OsiCpxSolverInterface::setCoefficient()`
   *
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @param value New coefficient value
   */
  void set_coeff(RowIndex row, ColIndex column, double value);

  /**
   * @brief Checks whether the solver supports in-place coefficient updates
   *
   * Returns true for CLP, CBC, and CPLEX solvers that provide
   * `modifyCoefficient()` or equivalent.  When false, the caller should
   * fall back to a static coefficient (e.g. mean efficiency).
   *
   * @return true if set_coeff() is functional
   */
  static bool supports_set_coeff() noexcept;

  void set_obj_coeff(ColIndex index, double value);
  [[nodiscard]] constexpr auto get_obj_coeff() const
  {
    return std::span(solver->getObjCoefficients(), get_numcols());
  }

  void set_col_low(ColIndex index, double value);
  void set_col_upp(ColIndex index, double value);
  void set_col(ColIndex index, double value);

  [[nodiscard]] double get_obj_value() const;

  /**
   * @brief Writes the problem to an LP format file
   * @param filename Name of the file to write (without extension)
   */
  void write_lp(const std::string& filename) const;

  /**
   * @brief Performs initial solve of the problem from scratch
   * @param solver_options Options controlling the solve process
   * @return True if the solve was successful, false otherwise
   */
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
   * @return Condition number kappa, or -1 if not available
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
  [[nodiscard]] constexpr auto get_row_low() const
  {
    return std::span(solver->getRowLower(), get_numrows());
  }

  /**
   * @brief Gets the upper bounds for all constraint rows
   * @return Span view of row upper bounds
   */
  [[nodiscard]] constexpr auto get_row_upp() const
  {
    return std::span(solver->getRowUpper(), get_numrows());
  }

  /**
   * @brief Gets the lower bounds for all variable columns
   * @return Span view of column lower bounds
   */
  [[nodiscard]] constexpr auto get_col_low() const
  {
    return std::span(solver->getColLower(), get_numcols());
  }

  /**
   * @brief Gets the upper bounds for all variable columns
   * @return Span view of column upper bounds
   */
  [[nodiscard]] constexpr auto get_col_upp() const
  {
    return std::span(solver->getColUpper(), get_numcols());
  }

  /**
   * @brief Gets the solution values for all variables
   * @return Span view of solution values
   */
  [[nodiscard]] constexpr auto get_col_sol() const
  {
    return std::span(solver->getColSolution(), get_numcols());
  }

  /**
   * @brief Gets the reduced costs for all variables
   * @return Span view of reduced costs
   */
  [[nodiscard]] constexpr auto get_col_cost() const
  {
    return std::span(solver->getReducedCost(), get_numcols());
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
  [[nodiscard]] constexpr auto get_row_dual() const
  {
    return std::span(solver->getRowPrice(), get_numrows());
  }

  void set_col_sol(std::span<const double> sol);
  void set_row_dual(std::span<const double> dual);

  void set_log_file(const std::string& plog_file);
  [[nodiscard]] constexpr const auto& get_log_file() const { return log_file; }

  void set_prob_name(const std::string& pname);
  [[nodiscard]] std::string get_prob_name() const;

  /// @name LP coefficient statistics (populated during load_flat from
  ///       FlatLinearProblem::stats_* fields, which are computed in
  ///       LinearProblem::to_flat when FlatOptions::compute_stats is true).
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
  void set_solver_opts(const SolverOptions& solver_options);

  RowIndex add_row(const std::string& name,
                   size_t numberElements,
                   const std::span<const int>& columns,
                   const std::span<const double>& elements,
                   double rowlb,
                   double rowub);

  void open_log_handler(int log_level);
  void close_log_handler();

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

  solver_ptr_t solver;
  std::string log_file {};

  std::vector<double> m_col_scales_;  ///< Per-column physical-to-LP scale
                                      ///< factors (physical = LP × scale)

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
  log_file_ptr_t log_file_ptr;
  std::unique_ptr<CoinMessageHandler> handler {};
};

}  // namespace gtopt
