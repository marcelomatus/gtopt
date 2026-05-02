/**
 * @file      highs_solver_backend.hpp
 * @brief     HiGHS native solver backend
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>

class Highs;

namespace gtopt
{

/// Cached state used to replay options + logging + prob name onto a HiGHS
/// instance.  Mirrors the CPLEX plugin's CplexPrep: clone() and any future
/// recreate-on-load path copy this cache and apply its fields to the new
/// Highs instance, keeping backend state consistent with the source.
struct HighsPrep
{
  std::optional<SolverOptions> options {};
  std::string log_filename {};
  int log_level {0};
  std::string prob_name {};
};

/**
 * @brief Solver backend using the HiGHS native C++ API.
 *
 * Uses the Highs class directly (no OSI wrapper), providing access to
 * all HiGHS features including IPM, simplex, and MIP solving.
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class HighsSolverBackend final : public SolverBackend
{
public:
  HighsSolverBackend();
  ~HighsSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

  /// Plugin-level infinity constant — single source of truth shared
  /// with the instance method `infinity()` and the plugin entry
  /// `gtopt_solver_infinity`.  HiGHS uses `kHighsInf = 1e+30`.
  static double plugin_infinity() noexcept;

  // ---- problem name ----
  void set_prob_name(const std::string& name) override;
  [[nodiscard]] std::string get_prob_name() const override;

  // ---- bulk load ----
  void load_problem(int ncols,
                    int nrows,
                    const int* matbeg,
                    const int* matind,
                    const double* matval,
                    const double* collb,
                    const double* colub,
                    const double* obj,
                    const double* rowlb,
                    const double* rowub) override;

  // ---- dimensions ----
  [[nodiscard]] int get_num_cols() const override;
  [[nodiscard]] int get_num_rows() const override;

  // ---- column ops ----
  void add_col(double lb, double ub, double obj) override;
  void add_cols(int num_cols,
                const int* colbeg,
                const int* colind,
                const double* colval,
                const double* collb,
                const double* colub,
                const double* colobj) override;
  void set_col_lower(int index, double value) override;
  void set_col_upper(int index, double value) override;
  void set_obj_coeff(int index, double value) override;
  void set_obj_coeffs(const double* values, int num_cols) override;

  // ---- row ops ----
  void add_row(int num_elements,
               const int* columns,
               const double* elements,
               double rowlb,
               double rowub) override;
  void add_rows(int num_rows,
                const int* rowbeg,
                const int* rowind,
                const double* rowval,
                const double* rowlb,
                const double* rowub) override;
  void set_row_lower(int index, double value) override;
  void set_row_upper(int index, double value) override;
  void set_row_bounds(int index, double lb, double ub) override;
  void delete_rows(int num, const int* indices) override;

  // ---- coefficients ----
  [[nodiscard]] double get_coeff(int row, int col) const override;
  void set_coeff(int row, int col, double value) override;
  [[nodiscard]] bool supports_set_coeff() const noexcept override;

  // ---- variable types ----
  void set_continuous(int index) override;
  void set_integer(int index) override;
  [[nodiscard]] bool is_continuous(int index) const override;
  [[nodiscard]] bool is_integer(int index) const override;

  // ---- solution access ----
  [[nodiscard]] const double* col_lower() const override;
  [[nodiscard]] const double* col_upper() const override;
  [[nodiscard]] const double* obj_coefficients() const override;
  [[nodiscard]] const double* row_lower() const override;
  [[nodiscard]] const double* row_upper() const override;
  [[nodiscard]] const double* col_solution() const override;
  [[nodiscard]] const double* reduced_cost() const override;
  [[nodiscard]] const double* row_price() const override;
  [[nodiscard]] double obj_value() const override;

  // ---- solution hints ----
  void set_col_solution(const double* sol) override;
  void set_row_price(const double* price) override;

  // ---- solve ----
  void initial_solve() override;
  void resolve() override;

  // ---- robust-solve mode ----
  void engage_robust_solve() override;
  void disengage_robust_solve() noexcept override;

  // ---- status ----
  [[nodiscard]] bool is_proven_optimal() const override;
  [[nodiscard]] bool is_abandoned() const override;
  [[nodiscard]] bool is_proven_primal_infeasible() const override;
  [[nodiscard]] bool is_proven_dual_infeasible() const override;
  // ---- options ----
  void apply_options(const SolverOptions& opts) override;
  [[nodiscard]] SolverOptions optimal_options() const override;
  [[nodiscard]] LPAlgo get_algorithm() const override;
  [[nodiscard]] int get_threads() const override;
  [[nodiscard]] bool get_presolve() const override;
  [[nodiscard]] int get_log_level() const override;

  // ---- diagnostics ----
  [[nodiscard]] std::optional<double> get_kappa() const override;

  // ---- logging ----
  void open_log(FILE* file, int level) override;
  void close_log() override;
  void set_log_filename(const std::string& filename, int level) override;
  void clear_log_filename() override;

  // ---- names ----
  void push_names(const std::vector<std::string>& col_names,
                  const std::vector<std::string>& row_names) override;
  void write_lp(const char* filename) override;

  // ---- clone ----
  [[nodiscard]] std::unique_ptr<SolverBackend> clone() const override;

private:
  std::unique_ptr<Highs> m_highs_;
  HighsPrep m_prep_;

  // Cached option values (updated by apply_options)
  LPAlgo m_algorithm_ {LPAlgo::default_algo};
  int m_threads_ {0};
  bool m_presolve_ {true};
  int m_log_level_ {0};
  // Cached solution vectors (HiGHS returns by reference, we store copies)
  mutable std::vector<double> m_col_solution_;
  mutable std::vector<double> m_col_dual_;
  mutable std::vector<double> m_row_dual_;
  mutable bool m_solution_valid_ {};
  bool m_load_failed_ {};

  /// Snapshot of HiGHS tolerances + presolve/scaling captured by
  /// engage_robust_solve().  The first engage records the baseline;
  /// disengage restores those exact values.
  struct RobustState
  {
    double primal_feasibility_tolerance {};
    double dual_feasibility_tolerance {};
    double ipm_optimality_tolerance {};
    std::string presolve {"on"};
    int simplex_scale_strategy {4};
    int engage_count {0};
  };
  std::optional<RobustState> m_saved_robust_state_;

  void cache_solution() const;
};

}  // namespace gtopt
