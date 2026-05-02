/**
 * @file      cplex_solver_backend.hpp
 * @brief     CPLEX native solver backend via C Callable Library
 * @date      Tue Mar 25 2026
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

// Forward-declare opaque CPLEX types
struct cpxenv;
struct cpxlp;

namespace gtopt
{

/// Cached state used to (re)prepare a CPLEX env + lp pair.  The backend
/// updates this whenever the caller applies options, log settings, or a
/// problem name; CplexEnvLp's constructor consumes it as the single
/// point where an env and its lp are opened and configured.
struct CplexPrep
{
  std::optional<SolverOptions> options {};
  std::string log_filename {};
  int log_level {0};
  std::string prob_name {};
};

/// RAII owner of one CPLEX environment + one CPLEX problem.  Each
/// CplexSolverBackend holds exactly one; when a fresh env+lp is needed
/// (load_problem, clone) the member is move-assigned from a new
/// CplexEnvLp, which destroys the previous pair and runs all env/lp
/// preparation in its constructor.
class CplexEnvLp
{
public:
  /// Silent, option-free env with an empty "gtopt" problem.
  CplexEnvLp();

  /// Silent env with full preparation applied (options, log file, name).
  explicit CplexEnvLp(const CplexPrep& prep);

  ~CplexEnvLp();

  CplexEnvLp(const CplexEnvLp&) = delete;
  CplexEnvLp& operator=(const CplexEnvLp&) = delete;
  CplexEnvLp(CplexEnvLp&& other) noexcept;
  CplexEnvLp& operator=(CplexEnvLp&& other) noexcept;

  [[nodiscard]] cpxenv* env() const noexcept { return m_env_; }
  [[nodiscard]] cpxlp* lp() const noexcept { return m_lp_; }

  /// Release the held lp so a clone replacement can be installed.
  /// Intended for CplexSolverBackend::clone() only.
  void reset_lp(cpxlp* new_lp) noexcept;

private:
  cpxenv* m_env_ {};
  cpxlp* m_lp_ {};
};

/**
 * @brief Solver backend using the CPLEX C Callable Library directly.
 *
 * Uses CPXopenCPLEX / CPXcreateprob / CPXcopyLP / CPXlpopt etc.
 * from ``<ilcplex/cplex.h>`` without any OSI wrapper.
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class CplexSolverBackend final : public SolverBackend
{
public:
  CplexSolverBackend();
  ~CplexSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

  /// Plugin-level infinity constant — exposed so the plugin's
  /// `gtopt_solver_infinity` entry point can return the SAME value
  /// without instantiating a `CplexSolverBackend`.  The instance
  /// method `infinity()` returns this constant too, making it the
  /// single source of truth.  CPLEX uses `CPX_INFBOUND = 1e+20`.
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
  // ---- solver options ----
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

  // ---- names & LP output ----
  void push_names(const std::vector<std::string>& col_names,
                  const std::vector<std::string>& row_names) override;
  void write_lp(const char* filename) override;

  // ---- deep copy ----
  [[nodiscard]] std::unique_ptr<SolverBackend> clone() const override;

private:
  void cache_problem_data() const;
  void cache_solution() const;

  /// Re-open env+lp from scratch with all cached preparation applied.
  void reset_env_lp();

  CplexEnvLp m_env_lp_;
  CplexPrep m_prep_;

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
  // Cached option values (updated by apply_options)
  LPAlgo m_algorithm_ {LPAlgo::default_algo};
  int m_threads_ {0};
  bool m_presolve_ {true};
  int m_log_level_ {0};

  /// Snapshot of the CPLEX numerical-robustness parameters captured by
  /// engage_robust_solve().  When set, disengage_robust_solve() restores
  /// the saved values.  Repeated engage calls keep this snapshot
  /// (so the very first baseline survives the chain) and simply
  /// loosen the tolerances another × 10.
  struct RobustState
  {
    double epopt {};
    double eprhs {};
    double barepcomp {};
    int numerical_emphasis {};
    int perind {};
    int repeat_presolve {};
    int engage_count {0};
  };
  std::optional<RobustState> m_saved_robust_state_;
};

}  // namespace gtopt
