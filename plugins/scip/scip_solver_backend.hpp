/**
 * @file      scip_solver_backend.hpp
 * @brief     SCIP (open-source MIP) solver backend — buffer-and-replay adapter
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * SCIP is a strong open-source constraint-integer-programming / MIP solver
 * (Apache-2.0 since v9) with a staged C API: you build an immutable original
 * problem (`SCIPcreate` + `SCIPincludeDefaultPlugins` + `SCIPcreateProbBasic`,
 * one `SCIPcreateVarBasic`/`SCIPcreateConsBasicLinear` per column/row), call
 * `SCIPsolve`, read the primal from `SCIPgetBestSol`, then `SCIPfree`.  SCIP
 * has NO convenient live-mutation entry point that survives the PROBLEM ->
 * TRANSFORMED -> SOLVED stage transitions.
 *
 * gtopt's `SolverBackend`, by contrast, is incrementally mutated between solves
 * (`set_col_lower`, `set_coeff`, `add_row`, `delete_rows`, ...).  This backend
 * therefore mirrors the cuOpt plugin's design: it keeps its OWN mutable model
 * in host memory (`ScipModel`) and **rebuilds the SCIP problem from scratch on
 * every solve** (buffer-and-replay).  All mutators write into `ScipModel`;
 * `solve_()` lowers it to SCIP, solves, snapshots primal/dual/obj/status into
 * host vectors that the pointer-getters return, then frees the SCIP instance.
 *
 * Consequences (all acceptable for gtopt — gtopt is documented cold-start):
 *   - No warm start (project memory: `no-warm-start`).
 *   - `clone()` deep-copies `ScipModel` (an independent host buffer) — exactly
 *     what the SDDP aperture/forward clone paths need, and parallel-safe.
 *   - `set_mip_start(repair)` buffers the candidate; `solve_()` installs it as
 * a SCIP *partial* solution and turns on the completesol/repair heuristic so
 *     SCIP completes/repairs it into a feasible integer incumbent — the
 *     mechanism `scip_repair_candidate` (source/mip_start_scip.cpp) relies on.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>

namespace gtopt
{

/**
 * @brief In-memory mutable LP/MIP model lowered to SCIP on every solve.
 *
 * Storage is row-major (CSR-friendly): each row owns a sparse map of
 * (column -> coefficient).  Single-element `set_coeff` overrides update the
 * matching entry.  Mirrors the cuOpt backend's `CuOptModel`.
 */
struct ScipModel
{
  int num_cols {0};
  int num_rows {0};

  // ---- columns ----
  std::vector<double> col_lb {};
  std::vector<double> col_ub {};
  std::vector<double> col_obj {};
  std::vector<char> col_type {};  // 'C' continuous, 'I' integer

  // ---- rows (ranged: rl <= a.x <= ru) ----
  std::vector<double> row_lb {};
  std::vector<double> row_ub {};
  std::vector<std::map<int, double>> row_entries {};

  [[nodiscard]] ScipModel clone() const { return *this; }
};

/// Solution snapshot taken after a solve so the const pointer-getters return
/// stable host memory independent of the (already-freed) SCIP instance.
struct ScipSolutionCache
{
  std::vector<double> primal {};
  std::vector<double> dual {};  // row prices (pure-LP only; 0 for MIP)
  std::vector<double> reduced {};  // reduced costs (pure-LP only; 0 for MIP)
  double obj {0.0};
  int status {0};  // SCIP_STATUS_* (0 == SCIP_STATUS_UNKNOWN)
  bool solved {false};
};

/**
 * @brief Solver backend driving SCIP via its C API (buffer-and-replay).
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class ScipSolverBackend final : public SolverBackend
{
public:
  ScipSolverBackend();
  ~ScipSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

  /// Plugin-level infinity — shared with `gtopt_solver_infinity`.  SCIP's
  /// native infinity is 1e20, but (like the cuOpt/HiGHS plugins) we report the
  /// conventional 1e30 sentinel so gtopt's `auto_scale_*` "no bound" detection
  /// behaves identically across backends; `solve_()` clamps to SCIPinfinity().
  static double plugin_infinity() noexcept;

  // ---- problem name ----
  void set_prob_name(const std::string& name) override;
  [[nodiscard]] std::string get_prob_name() const override;

  // ---- bulk load (CSC from FlatLinearProblem) ----
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
  int relax_all_integers() override;

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
  /// MIP start: buffered here, installed as a SCIP partial solution inside
  /// `solve_()`.  Under `effort == repair` the completesol heuristic completes
  /// /repairs it into a feasible incumbent.
  bool set_mip_start(std::span<const double> col_values,
                     MipStartEffort effort) override;

  // ---- solve ----
  void initial_solve() override;
  void resolve() override;
  [[nodiscard]] SolveEffort last_solve_effort() const override;

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

  // ---- names & LP output ----
  void push_names(const std::vector<std::string>& col_names,
                  const std::vector<std::string>& row_names) override;
  void write_lp(const char* filename) override;

  // ---- clone ----
  [[nodiscard]] std::unique_ptr<SolverBackend> clone() const override;

private:
  /// Lower `m_model_` to a fresh SCIP instance, solve, and snapshot the
  /// primal/dual/obj/status into `m_sol_`.  Shared by `initial_solve` and
  /// `resolve` (both rebuild from scratch — SCIP has no warm path here).
  void solve_();

  ScipModel m_model_ {};
  ScipSolutionCache m_sol_ {};
  SolveEffort m_last_effort_ {};
  SolverOptions m_options_ {};

  std::vector<double> m_mip_start_ {};  ///< buffered MIP start (set_mip_start)
  MipStartEffort m_mip_start_effort_ {MipStartEffort::check_feasibility};

  std::string m_prob_name_ {"gtopt_scip"};
  std::vector<std::string> m_col_names_ {};
  std::vector<std::string> m_row_names_ {};
  int m_log_level_ {0};
  std::string m_log_filename_ {};  ///< "<stem>.log" set when log_mode != nolog

  std::optional<SolverOptions> m_saved_robust_options_;
};

}  // namespace gtopt
