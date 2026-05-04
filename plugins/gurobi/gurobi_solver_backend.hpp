/**
 * @file      gurobi_solver_backend.hpp
 * @brief     Gurobi native solver backend via C API
 * @date      Wed Apr 16 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>

// Gurobi opaque types (forward declared — real defs are typedef struct ...)
struct _GRBenv;
struct _GRBmodel;
using GRBenv = struct _GRBenv;
using GRBmodel = struct _GRBmodel;

namespace gtopt
{

/// Cached state used to replay options + logging + prob name onto a
/// freshly recreated Gurobi model.  Mirrors the MindOpt plugin's
/// MindOptPrep: clone() and load_problem() paths read this cache and
/// apply its fields to the new (env,model) pair so backend state
/// survives a deep-copy clone or a load_problem() cycle.
struct GurobiPrep
{
  std::optional<SolverOptions> options {};
  std::string log_filename {};
  int log_level {0};
  std::string prob_name {};
};

/**
 * @brief Solver backend using the Gurobi C API.
 *
 * Uses GRBloadenv / GRBnewmodel / GRBoptimize etc. from gurobi_c.h.
 * Gurobi supports ranged constraints natively via GRBaddrangeconstr().
 *
 * Gurobi uses lazy model updates: new vars/constraints are not visible
 * to queries until GRBupdatemodel() is called.  We track a dirty flag
 * and update on demand before reading model state.
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class GurobiSolverBackend final : public SolverBackend
{
public:
  GurobiSolverBackend();
  ~GurobiSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

  /// Plugin-level infinity constant — single source of truth shared
  /// with the instance method `infinity()` and the plugin entry
  /// `gtopt_solver_infinity`.  Gurobi uses `GRB_INFINITY = 1e+30`.
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

  // ---- solution access (span-out) ----
  // Override to call GRBgetdblattrarray directly into the caller's
  // buffer — bypasses the per-instance scratch members
  // (`m_col_solution_/m_reduced_cost_/m_row_price_`), which are now
  // off-mode-only.
  void fill_col_sol(std::span<double> out) const override;
  void fill_col_cost(std::span<double> out) const override;
  void fill_row_dual(std::span<double> out) const override;

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
  /// Per-member lazy fillers for the problem-data getters.  Each fills
  /// its own buffer on demand and trips its own cached flag.  Mutations
  /// invalidate every flag via `invalidate_problem_data()` so the next
  /// read re-queries Gurobi — but only for the buffer the caller asks
  /// for, sparing the unread members.  Production paths read only
  /// `col_lower`/`col_upper` (via `LinearInterface::ScaledView`); the
  /// `obj_coefficients` member is populated only when test paths
  /// request it.  Row-bound members live in `m_rowlb_local_` /
  /// `m_rowub_local_` and are maintained directly (no flag needed).
  void fill_collb_if_needed() const;
  void fill_colub_if_needed() const;
  void fill_obj_if_needed() const;
  void invalidate_problem_data() const noexcept;
  void check_error(int rc, const char* func) const;

  /// Flush any pending model changes via GRBupdatemodel if the dirty
  /// flag is set.  Gurobi is lazy — NumVars / NumConstrs / reads of
  /// newly-added elements see stale data until this is called.
  void ensure_updated_() const;

  /// Recreate m_model_ from m_prep_.  Used by load_problem() so every
  /// bulk load starts with a pristine model — mirrors the MindOpt
  /// plugin's reset_model_() pattern.
  void reset_model_();

  GRBenv* m_env_ {};
  GRBmodel* m_model_ {};

  /// Cache of everything needed to replay backend state onto a fresh
  /// (env,model) pair (see MindOptPrep for the pattern).
  GurobiPrep m_prep_;

  /// Gurobi uses lazy updates — flip this when any mutating op is
  /// issued, and call GRBupdatemodel before any read.
  mutable bool m_dirty_ {false};

  // Per-member problem-data caches.  Only the three Gurobi-attribute-
  // backed members need flags; row bounds live in m_rowlb_local_/
  // m_rowub_local_ (we maintain them ourselves because Gurobi range
  // constraints are awkward to read back via attributes).
  mutable bool m_collb_cached_ {false};
  mutable bool m_colub_cached_ {false};
  mutable bool m_obj_cached_ {false};
  mutable std::vector<double> m_collb_;
  mutable std::vector<double> m_colub_;
  mutable std::vector<double> m_obj_;

  /// Row bounds tracked locally: Gurobi range constraints are
  /// represented via a slack variable internally, so reading them back
  /// via attributes is awkward.  We maintain the authoritative copy.
  std::vector<double> m_rowlb_local_;
  std::vector<double> m_rowub_local_;

  /// C-API write target for `col_solution()` / `reduced_cost()` /
  /// `row_price()`.  Each accessor refills *only its own* buffer on
  /// every call (via the matching GRBgetdblattrarray) — this is plain
  /// scratch storage, not a cache: there is no validity flag, no
  /// caching semantics, and no cross-accessor refill.  The single
  /// caching layer is in
  /// `LinearInterface::populate_solution_cache_post_solve`.
  mutable std::vector<double> m_col_solution_;
  mutable std::vector<double> m_reduced_cost_;
  mutable std::vector<double> m_row_price_;

  // Cached option values (updated by apply_options)
  LPAlgo m_algorithm_ {LPAlgo::default_algo};
  int m_threads_ {0};
  bool m_presolve_ {true};
  int m_log_level_ {0};

  /// Snapshot of Gurobi numerical parameters captured by
  /// engage_robust_solve().
  struct RobustState
  {
    double optimality_tol {};
    double feasibility_tol {};
    double bar_conv_tol {};
    int numeric_focus {};
    int engage_count {0};
  };
  std::optional<RobustState> m_saved_robust_state_;
};

}  // namespace gtopt
