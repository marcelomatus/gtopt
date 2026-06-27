/**
 * @file      cuopt_solver_backend.hpp
 * @brief     NVIDIA cuOpt (GPU) solver backend — buffer-and-replay adapter
 * @date      Tue Jun 10 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * cuOpt exposes a **build-once / solve-once / destroy** C API
 * (``cuopt/linear_programming/cuopt_c.h``): you assemble an immutable
 * ``cuOptOptimizationProblem`` from a CSR matrix + bounds + var types,
 * call ``cuOptSolve``, read the primal / dual / reduced-cost vectors, and
 * destroy it.  There is **no** live-mutation entry point (no equivalent of
 * CPLEX ``CPXchgbds`` / MindOpt ``MDOsetdblattrelement`` /
 * HiGHS ``changeColBounds``).
 *
 * gtopt's `SolverBackend`, by contrast, is a *live, incrementally mutated*
 * object: `LinearInterface` calls `set_col_lower`, `set_coeff`, `add_row`,
 * `delete_rows`, `set_obj_coeff`, … BETWEEN solves on an already-solved
 * backend (notably in `LowMemoryMode::off`, where the LI cache is empty and
 * the backend IS the source of truth — see
 * `source/sddp_forward_pass.cpp`, `source/sddp_aperture.cpp`,
 * `source/system_lp.cpp::update_lp`).
 *
 * This backend therefore keeps its OWN mutable model in host memory
 * (`Model` below: CSR-by-row triplets + bounds + obj + var types) and
 * **rebuilds the cuOpt problem object from scratch on every `resolve()` /
 * `initial_solve()`**.  All mutators write into `Model`; `solve_()` lowers
 * `Model` to the dense arrays `cuOptCreateRangedProblem` wants, solves on
 * the GPU, and snapshots primal/dual/reduced-cost/obj into host vectors that
 * the pointer-getters return.
 *
 * Consequences (all acceptable for gtopt):
 *   - No warm start.  gtopt is documented cold-start (barrier) on every
 *     resolve, so nothing is lost (project memory: `no-warm-start`).
 *   - `supports_set_coeff()` returns TRUE (we buffer the override into the
 *     CSR), so SDDP's volume-dependent coefficient updates in
 *     `update_lp()` are NOT silently dropped.
 *   - `clone()` deep-copies `Model` (an independent host buffer) — exactly
 *     what the SDDP aperture/forward clone paths need.
 *
 * STATUS: SCAFFOLD.  The Model + mutator buffering + CSR lowering +
 * solve/getter wiring are implemented and compile-probed in isolation.
 * It is NOT yet validated end-to-end against the full gtopt LP pipeline or
 * a GPU solve.  See the step-by-step plan at the bottom of
 * `cuopt_solver_backend.cpp`.
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
 * @brief In-memory, mutable LP/MIP model assembled by the gtopt
 * `SolverBackend` mutators and lowered to cuOpt's immutable problem on solve.
 *
 * Storage is row-major (CSR-friendly): each row owns a sparse list of
 * (column, value) entries.  Single-element `set_coeff` overrides update the
 * matching entry (or append one).  This mirrors gtopt's own
 * `FlatLinearProblem` so the lowering step is a flat copy.
 */
struct CuOptModel
{
  // ---- dimensions ----
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
  // Per-row sparse entries (column index → coefficient).  std::map keeps
  // columns sorted and makes `set_coeff` overrides O(log nnz_row); a flat
  // vector + sort at lowering time is a later optimization.
  std::vector<std::map<int, double>> row_entries {};

  // Objective constant offset (gtopt scale/FCF offsets).  cuOpt accepts it
  // directly via the `objective_offset` argument.
  double obj_offset {0.0};

  [[nodiscard]] CuOptModel clone() const { return *this; }
};

/// Cached solution vectors snapshot after a solve, so the pointer-getters
/// (`col_solution()`, `row_price()`, `reduced_cost()`, `obj_value()`) return
/// stable host memory independent of the (already-destroyed) cuOpt solution
/// object.
struct CuOptSolutionCache
{
  std::vector<double> primal {};
  std::vector<double> dual {};  // row prices (one per row)
  std::vector<double> reduced {};  // reduced costs (one per col)
  double obj {0.0};
  int termination {0};  // CUOPT_TERMINATION_STATUS_*
  bool solved {false};
};

/**
 * @brief Solver backend driving NVIDIA cuOpt via its stable C API.
 *
 * @note Single-GPU; concurrent use across threads must serialize at the
 *       gtopt clone level (each clone owns an independent `CuOptModel`).
 */
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions,hicpp-special-member-functions)
class CuOptSolverBackend final : public SolverBackend
{
public:
  CuOptSolverBackend();
  ~CuOptSolverBackend() override;

  // ---- identity ----
  [[nodiscard]] std::string_view solver_name() const noexcept override;
  [[nodiscard]] std::string solver_version() const override;
  [[nodiscard]] double infinity() const noexcept override;
  [[nodiscard]] bool supports_mip() const noexcept override;

  /// Plugin-level infinity — shared with `gtopt_solver_infinity`.
  /// cuOpt uses `CUOPT_INFINITY = std::numeric_limits<double>::infinity()`,
  /// but we report the conventional 1e30 sentinel so gtopt's
  /// `auto_scale_*` "no bound" detection works identically to HiGHS/OSI.
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

  // ---- solution hints (warm start; no-op — cuOpt rebuilds cold) ----
  void set_col_solution(const double* sol) override;
  void set_row_price(const double* price) override;

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
  /// Lower `m_model_` to cuOpt's CSR arrays, solve on the GPU, and snapshot
  /// primal/dual/reduced-cost/obj into `m_sol_`.  Shared by `initial_solve`
  /// and `resolve` (both rebuild from scratch — cuOpt has no warm path).
  void solve_();

  CuOptModel m_model_ {};
  CuOptSolutionCache m_sol_ {};
  SolveEffort m_last_effort_ {};  // GPU solve time (ticks=time) of last solve
  SolverOptions m_options_ {};
  std::string m_prob_name_ {"gtopt_cuopt"};
  std::string m_log_filename_ {};  ///< CUOPT_LOG_FILE path (set_log_filename)

  // Scratch buffers reused across getters so the const pointer-getters can
  // hand back stable storage.  `m_model_` already owns col_lb/col_ub/etc.
  // directly, so those getters return into `m_model_`; only the solution
  // vectors live in `m_sol_`.

  // Robust-solve tolerance snapshot (restored by disengage).
  std::optional<SolverOptions> m_saved_robust_options_;
};

}  // namespace gtopt
