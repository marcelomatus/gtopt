/**
 * @file      mindopt_solver_backend.hpp
 * @brief     MindOpt native solver backend via C API
 * @date      Fri Apr  4 2026
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

// MindOpt opaque types (typedef void)
using MDOenv = void;
using MDOmodel = void;

namespace gtopt
{

/// Cached state used to replay options + logging + prob name onto a
/// freshly recreated MindOpt model.  Mirrors the CPLEX plugin's CplexPrep:
/// clone() and load_problem() paths read this cache and apply its fields
/// to the new (env,model) pair so backend state survives a deep-copy
/// clone or a load_problem() cycle.
struct MindOptPrep
{
  std::optional<SolverOptions> options {};
  std::string log_filename {};
  int log_level {0};
  std::string prob_name {};
};

/**
 * @brief Solver backend using the MindOpt C API (Alibaba DAMO Academy).
 *
 * Uses MDOloadenv / MDOnewmodel / MDOoptimize etc. from Mindopt.h.
 * MindOpt uses ranged constraints natively via MDOaddrangeconstr().
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class MindOptSolverBackend final : public SolverBackend
{
public:
  MindOptSolverBackend();
  ~MindOptSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

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

  // ---- status ----
  [[nodiscard]] bool is_proven_optimal() const override;
  [[nodiscard]] bool is_abandoned() const override;
  [[nodiscard]] bool is_proven_primal_infeasible() const override;
  [[nodiscard]] bool is_proven_dual_infeasible() const override;
  // ---- solver options ----
  void apply_options(const SolverOptions& opts) override;
  [[nodiscard]] SolverOptions optimal_options() const override;
  [[nodiscard]] LPAlgo get_algorithm() const override;
  [[nodiscard]] int get_threads() const override;
  [[nodiscard]] bool get_presolve() const override;
  [[nodiscard]] int get_log_level() const override;

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
  static void check_error(int rc, const char* func);

  /// Recreate m_model_ from m_prep_ (env-level params were already applied
  /// onto m_env_ by apply_options_to_env).  Used by load_problem() so every
  /// bulk load starts with a pristine model — mirrors the CPLEX plugin's
  /// reset_env_lp() pattern.
  void reset_model_();

  MDOenv* m_env_ {};
  MDOmodel* m_model_ {};

  /// Cache of everything needed to replay backend state onto a fresh
  /// (env,model) pair (see CplexPrep for the pattern).
  MindOptPrep m_prep_;

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

  // Cached option values (updated by apply_options)
  LPAlgo m_algorithm_ {LPAlgo::default_algo};
  int m_threads_ {0};
  bool m_presolve_ {true};
  int m_log_level_ {0};
};

}  // namespace gtopt
