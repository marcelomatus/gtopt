/**
 * @file      solver_backend.hpp
 * @brief     Abstract solver backend interface for LP/MIP solvers
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the pure virtual interface that all solver plugins must implement.
 * This header has zero dependencies on any solver library (COIN-OR, HiGHS,
 * CPLEX, etc.) — concrete implementations live in dynamically loaded plugins.
 */

#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gtopt
{

struct SolverOptions;

/**
 * @brief Abstract interface for LP/MIP solver backends.
 *
 * Each solver plugin (CLP, CBC, CPLEX, HiGHS, …) provides a concrete
 * implementation of this class.  LinearInterface delegates all solver
 * operations through this interface, enabling runtime solver selection
 * via dlopen-loaded plugins.
 */
class SolverBackend
{
public:
  virtual ~SolverBackend() = default;

  SolverBackend() = default;
  SolverBackend(const SolverBackend&) = delete;
  SolverBackend& operator=(const SolverBackend&) = delete;
  SolverBackend(SolverBackend&&) = default;
  SolverBackend& operator=(SolverBackend&&) = default;

  // ---- identity ----

  /** @brief Human-readable solver name (e.g. "clp", "highs") */
  [[nodiscard]] virtual std::string_view solver_name() const noexcept = 0;

  /** @brief Solver library version string (e.g. "1.17.3", "22.1.1") */
  [[nodiscard]] virtual std::string solver_version() const = 0;

  /** @brief Solver's representation of +infinity */
  [[nodiscard]] virtual double infinity() const noexcept = 0;

  // ---- problem name ----

  virtual void set_prob_name(const std::string& name) = 0;
  [[nodiscard]] virtual std::string get_prob_name() const = 0;

  // ---- bulk load (CSC format from FlatLinearProblem) ----

  virtual void load_problem(int ncols,
                            int nrows,
                            const int* matbeg,
                            const int* matind,
                            const double* matval,
                            const double* collb,
                            const double* colub,
                            const double* obj,
                            const double* rowlb,
                            const double* rowub) = 0;

  // ---- dimensions ----

  [[nodiscard]] virtual int get_num_cols() const = 0;
  [[nodiscard]] virtual int get_num_rows() const = 0;

  // ---- column operations ----

  virtual void add_col(double lb, double ub, double obj) = 0;
  virtual void set_col_lower(int index, double value) = 0;
  virtual void set_col_upper(int index, double value) = 0;
  virtual void set_obj_coeff(int index, double value) = 0;

  // ---- row operations ----

  virtual void add_row(int num_elements,
                       const int* columns,
                       const double* elements,
                       double rowlb,
                       double rowub) = 0;
  virtual void set_row_lower(int index, double value) = 0;
  virtual void set_row_upper(int index, double value) = 0;
  virtual void set_row_bounds(int index, double lb, double ub) = 0;
  virtual void delete_rows(int num, const int* indices) = 0;

  // ---- coefficient access ----

  [[nodiscard]] virtual double get_coeff(int row, int col) const = 0;
  virtual void set_coeff(int row, int col, double value) = 0;
  [[nodiscard]] virtual bool supports_set_coeff() const noexcept = 0;

  // ---- variable types ----

  virtual void set_continuous(int index) = 0;
  virtual void set_integer(int index) = 0;
  [[nodiscard]] virtual bool is_continuous(int index) const = 0;
  [[nodiscard]] virtual bool is_integer(int index) const = 0;

  // ---- solution access (raw pointers, caller wraps in std::span) ----

  [[nodiscard]] virtual const double* col_lower() const = 0;
  [[nodiscard]] virtual const double* col_upper() const = 0;
  [[nodiscard]] virtual const double* obj_coefficients() const = 0;
  [[nodiscard]] virtual const double* row_lower() const = 0;
  [[nodiscard]] virtual const double* row_upper() const = 0;
  [[nodiscard]] virtual const double* col_solution() const = 0;
  [[nodiscard]] virtual const double* reduced_cost() const = 0;
  [[nodiscard]] virtual const double* row_price() const = 0;
  [[nodiscard]] virtual double obj_value() const = 0;

  // ---- solution hints (warm start) ----

  virtual void set_col_solution(const double* sol) = 0;
  virtual void set_row_price(const double* price) = 0;

  // ---- solve ----

  virtual void initial_solve() = 0;
  virtual void resolve() = 0;

  // ---- status ----

  [[nodiscard]] virtual bool is_proven_optimal() const = 0;
  [[nodiscard]] virtual bool is_abandoned() const = 0;
  [[nodiscard]] virtual bool is_proven_primal_infeasible() const = 0;
  [[nodiscard]] virtual bool is_proven_dual_infeasible() const = 0;

  // ---- solver options ----
  // Replaces all #ifdef COIN_USE_* / OSI_EXTENDED blocks.
  // Each backend translates SolverOptions to its native API.

  virtual void apply_options(const SolverOptions& opts) = 0;

  // ---- diagnostics ----

  /** @brief Condition number of the current basis (1.0 if unavailable) */
  [[nodiscard]] virtual double get_kappa() const = 0;

  // ---- logging ----

  virtual void open_log(FILE* file, int level) = 0;
  virtual void close_log() = 0;

  /** @brief Redirect solver log output to a file by name.
   *
   * Backends override this to use their native file-based logging API
   * (e.g. CPXsetlogfilename for CPLEX, log_file option for HiGHS).
   * The default implementation is a no-op — backends that only support
   * FILE*-based logging via open_log/close_log need not override.
   *
   * @param filename  Full path to the log file (without .log extension)
   * @param level     Verbosity level (0 = off, >0 = enabled)
   */
  virtual void set_log_filename(const std::string& /*filename*/, int /*level*/)
  {
  }

  /** @brief Stop file-based logging started by set_log_filename.
   *
   * Backends override to close/detach their native log file.
   * Default is a no-op.
   */
  virtual void clear_log_filename() {}

  // ---- names & LP file output ----

  virtual void push_names(const std::vector<std::string>& col_names,
                          const std::vector<std::string>& row_names) = 0;
  virtual void write_lp(const char* filename) = 0;

  // ---- deep copy ----

  [[nodiscard]] virtual std::unique_ptr<SolverBackend> clone() const = 0;
};

/// Plugin entry-point function type: creates a SolverBackend by solver name.
using solver_backend_factory_fn = SolverBackend* (*)(const char* solver_name);

/// Plugin name function type: returns the plugin name.
using solver_plugin_name_fn = const char* (*)();

/// Plugin solver-list function type: returns null-terminated array of names.
using solver_plugin_names_fn = const char* const* (*)();

}  // namespace gtopt
