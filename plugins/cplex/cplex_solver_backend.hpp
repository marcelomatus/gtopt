/**
 * @file      cplex_solver_backend.hpp
 * @brief     CPLEX native solver backend via C Callable Library
 * @date      Tue Mar 25 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <gtopt/solver_backend.hpp>

// Forward-declare opaque CPLEX types
struct cpxenv;
struct cpxlp;

namespace gtopt
{

/**
 * @brief Solver backend using the CPLEX C Callable Library directly.
 *
 * Uses CPXopenCPLEX / CPXcreateprob / CPXcopyLP / CPXlpopt etc.
 * from ``<ilcplex/cplex.h>`` without any OSI wrapper.
 */
class CplexSolverBackend final : public SolverBackend
{
public:
  CplexSolverBackend();
  ~CplexSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;

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
  void set_col_lower(int index, double value) override;
  void set_col_upper(int index, double value) override;
  void set_obj_coeff(int index, double value) override;

  // ---- row ops ----
  void add_row(int num_elements,
               const int* columns,
               const double* elements,
               double rowlb,
               double rowub) override;
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

  // ---- status ----
  [[nodiscard]] bool is_proven_optimal() const override;
  [[nodiscard]] bool is_abandoned() const override;
  [[nodiscard]] bool is_proven_primal_infeasible() const override;
  [[nodiscard]] bool is_proven_dual_infeasible() const override;

  // ---- solver options ----
  void apply_options(const SolverOptions& opts) override;

  // ---- diagnostics ----
  [[nodiscard]] double get_kappa() const override;

  // ---- logging ----
  void open_log(FILE* file, int level) override;
  void close_log() override;
  void set_log_filename(const std::string& filename, int level) override;
  void clear_log_filename() override;

  // ---- names & LP output ----
  void push_names(const std::vector<std::string>& col_names,
                  const std::vector<std::string>& row_names) override;
  void write_lp(const char* filename) override;

  // ---- deep copy ----
  [[nodiscard]] std::unique_ptr<SolverBackend> clone() const override;

private:
  void cache_problem_data() const;
  void cache_solution() const;

  cpxenv* m_env_ {};
  cpxlp* m_lp_ {};

  mutable bool m_prob_cached_ {false};
  mutable std::vector<double> m_collb_;
  mutable std::vector<double> m_colub_;
  mutable std::vector<double> m_obj_;
  mutable std::vector<double> m_rowlb_;
  mutable std::vector<double> m_rowub_;

  mutable bool m_sol_cached_ {false};
  mutable std::vector<double> m_col_solution_;
  mutable std::vector<double> m_reduced_cost_;
  mutable std::vector<double> m_row_price_;

  int m_solve_status_ {0};
};

}  // namespace gtopt
