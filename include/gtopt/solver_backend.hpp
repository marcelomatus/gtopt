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

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/solver_options.hpp>

namespace gtopt
{

/**
 * @brief ABI version for the SolverBackend plugin interface.
 *
 * Bump this integer every time the SolverBackend vtable changes
 * (new virtual methods, reordered methods, changed signatures, etc.).
 * SolverRegistry checks the plugin's reported ABI version at load time
 * and rejects incompatible plugins with a clear error instead of crashing.
 */
inline constexpr int k_solver_abi_version = 8;

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

  /** @brief True if this backend can solve mixed-integer problems.
   *
   * Returns true when the backend supports integer variables (set_integer)
   * and a branch-and-bound / branch-and-cut solver capable of resolving
   * them to optimality.  Pure LP backends such as CLP return false; CBC,
   * CPLEX, HiGHS, and MindOpt return true.
   *
   * Tests that exercise integer variables should skip when this query
   * returns false on every loaded plugin (see SolverRegistry::has_mip_solver).
   */
  [[nodiscard]] virtual bool supports_mip() const noexcept = 0;

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

  /// Bulk column addition in CSC (Compressed Sparse Column) format.
  ///
  /// Mirrors `add_rows` for columns: a single solver call replaces what
  /// would otherwise be `num_cols` per-column `add_col` invocations,
  /// avoiding repeated reallocation of the backend's internal column
  /// metadata.  Columns added by this path are continuous LP variables;
  /// callers needing integer columns should follow up with
  /// `set_integer(idx)`.
  ///
  /// @param num_cols  Number of columns to add
  /// @param colbeg    Column-start offsets into colind/colval
  ///                  (size num_cols+1, colbeg[num_cols] == nnz)
  /// @param colind    Row indices for non-zeros (size colbeg[num_cols])
  /// @param colval    Non-zero coefficient values (size colbeg[num_cols])
  /// @param collb     Column lower bounds (size num_cols)
  /// @param colub     Column upper bounds (size num_cols)
  /// @param colobj    Column objective coefficients (size num_cols)
  virtual void add_cols(int num_cols,
                        const int* colbeg,
                        const int* colind,
                        const double* colval,
                        const double* collb,
                        const double* colub,
                        const double* colobj) = 0;
  virtual void set_col_lower(int index, double value) = 0;
  virtual void set_col_upper(int index, double value) = 0;
  virtual void set_obj_coeff(int index, double value) = 0;

  /// Bulk update of all objective coefficients.  `values` MUST have size
  /// equal to the current number of columns; the implementation overwrites
  /// every coefficient.  Default fallback is a per-column loop over
  /// `set_obj_coeff(i, values[i])` so plugins that don't ship a native
  /// bulk API (or where the native bulk call has the same per-element
  /// cost) can opt out with one virtual.  Plugins SHOULD override when
  /// the native API is genuinely batched (CPXchgobj, HighsLp::col_cost,
  /// CoinPackedVector) — those drop the per-call dispatch overhead and
  /// often skip per-element bookkeeping.
  virtual void set_obj_coeffs(const double* values, int num_cols)
  {
    for (int i = 0; i < num_cols; ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      set_obj_coeff(i, values[i]);
    }
  }

  // ---- row operations ----

  virtual void add_row(int num_elements,
                       const int* columns,
                       const double* elements,
                       double rowlb,
                       double rowub) = 0;

  /// Bulk row addition in CSR (Compressed Sparse Row) format.
  ///
  /// @param num_rows  Number of rows to add
  /// @param rowbeg    Row-start offsets into rowind/rowval (size num_rows+1)
  /// @param rowind    Column indices for non-zeros
  /// @param rowval    Non-zero coefficient values
  /// @param rowlb     Row lower bounds (size num_rows)
  /// @param rowub     Row upper bounds (size num_rows)
  virtual void add_rows(int num_rows,
                        const int* rowbeg,
                        const int* rowind,
                        const double* rowval,
                        const double* rowlb,
                        const double* rowub) = 0;

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

  // ---- solution access (span-out: write into caller-owned buffer) ----
  //
  // Span-out variants used by `LinearInterface::populate_solution_cache_`
  // `post_solve` in compress / rebuild mode so the LI's `m_cached_*`
  // is the sole destination — no plugin scratch buffer is touched.
  // The off-mode read path keeps using the pointer-getters above.
  //
  // Naming mirrors the LI side:
  //   fill_col_sol  → m_cached_col_sol_   (primal solution)
  //   fill_col_cost → m_cached_col_cost_  (reduced cost / col dual)
  //   fill_row_dual → m_cached_row_dual_  (row price / row dual)
  //
  // Contract:
  //   * Caller sizes `out` to exactly `get_num_cols()` (resp.
  //     `get_num_rows()` for `fill_row_dual`); the plugin writes
  //     `out.size()` elements without bounds-checking.
  //   * Empty span → no-op (plugin must tolerate `out.size() == 0`).
  //   * No caching: every call re-queries the solver.  The plugin
  //     does not retain the buffer beyond the call.
  //
  // Default implementation (in this header) memcpy's from the
  // matching pointer-getter, so OSI and HiGHS — whose pointer-
  // getters return into solver-internal memory directly — need no
  // override.  CPLEX / MindOpt / Gurobi override to call the
  // C-API write-into-buffer entry points (CPXgetx/CPXgetdj/CPXgetpi,
  // MDOgetdblattrarray, GRBgetdblattrarray) directly with `out.data()`.
  virtual void fill_col_sol(std::span<double> out) const
  {
    if (const auto* p = col_solution(); p != nullptr) {
      std::copy_n(p, out.size(), out.data());
    }
  }
  virtual void fill_col_cost(std::span<double> out) const
  {
    if (const auto* p = reduced_cost(); p != nullptr) {
      std::copy_n(p, out.size(), out.data());
    }
  }
  virtual void fill_row_dual(std::span<double> out) const
  {
    if (const auto* p = row_price(); p != nullptr) {
      std::copy_n(p, out.size(), out.data());
    }
  }

  // ---- solution hints (warm start) ----

  virtual void set_col_solution(const double* sol) = 0;
  virtual void set_row_price(const double* price) = 0;

  // ---- solve ----

  virtual void initial_solve() = 0;
  virtual void resolve() = 0;

  // ---- robust-solve mode ----

  /// Switch the solver into a high-effort, numerically-conservative
  /// mode for the next solve.  Each backend internally:
  ///
  /// * Saves whichever native parameters it modifies.
  /// * Loosens the optimality / feasibility / barrier tolerances
  ///   (typical: × 10).
  /// * Enables solver-specific robustness knobs (CPLEX:
  ///   NUMERICALEMPHASIS=1, PERIND=1, REPEATPRESOLVE=3; HiGHS: enable
  ///   the high-accuracy IPM + scaling; CBC/CLP: tightened pivot
  ///   tolerances).
  ///
  /// May be called repeatedly — implementations should treat
  /// successive engages as additional "× 10" loosenings (or no-op,
  /// at the implementer's discretion) so the fallback chain can
  /// escalate.  Always paired with disengage_robust_solve() via the
  /// RobustSolveGuard RAII helper.
  virtual void engage_robust_solve() = 0;

  /// Restore parameters captured by engage_robust_solve.  No-op when
  /// engage was never called.  Safe to call from a destructor.
  virtual void disengage_robust_solve() noexcept = 0;

  // ---- status ----

  [[nodiscard]] virtual bool is_proven_optimal() const = 0;
  [[nodiscard]] virtual bool is_abandoned() const = 0;
  [[nodiscard]] virtual bool is_proven_primal_infeasible() const = 0;
  [[nodiscard]] virtual bool is_proven_dual_infeasible() const = 0;

  // ---- solver options ----
  // Replaces all #ifdef COIN_USE_* / OSI_EXTENDED blocks.
  // Each backend translates SolverOptions to its native API.

  virtual void apply_options(const SolverOptions& opts) = 0;

  /** @brief Return backend-optimal default options (benchmarked).
   *
   *  Called by LinearInterface before merging user-supplied options.
   *  Each backend returns its empirically best configuration; the caller
   *  overlays user-provided options on top so explicit settings always win.
   */
  [[nodiscard]] virtual SolverOptions optimal_options() const = 0;

  /** @brief Query the currently configured LP algorithm. */
  [[nodiscard]] virtual LPAlgo get_algorithm() const = 0;

  /** @brief Query the currently configured thread count (0 = solver default).
   */
  [[nodiscard]] virtual int get_threads() const = 0;

  /** @brief Query whether presolve is currently enabled. */
  [[nodiscard]] virtual bool get_presolve() const = 0;

  /** @brief Query the current solver log verbosity level (0 = off). */
  [[nodiscard]] virtual int get_log_level() const = 0;

  // ---- diagnostics ----

  /** @brief Condition number of the current basis.
   *
   *  Returns `std::nullopt` when the backend cannot compute it
   *  (e.g. no basis after a barrier solve without crossover, the
   *  underlying query failed, or the backend does not expose a
   *  basis-conditioning query at all).  Callers MUST NOT interpret
   *  a missing value as `1.0` — doing so silently poisons any
   *  `std::max`-based aggregation across a (scene, phase) grid.
   */
  [[nodiscard]] virtual std::optional<double> get_kappa() const = 0;

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
   * The first argument is the full path to the log file (without .log
   * extension); the second is the verbosity level (0 = off, >0 = enabled).
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

/// RAII helper that engages the solver's robust-solve mode for the
/// lifetime of the guard.  Construction calls SolverBackend::
/// engage_robust_solve(); destruction calls disengage_robust_solve().
/// Non-copyable, non-movable so the engage/disengage pair always
/// matches the surrounding scope.
class RobustSolveGuard
{
public:
  explicit RobustSolveGuard(SolverBackend& backend)
      : m_backend_(&backend)
  {
    m_backend_->engage_robust_solve();
  }

  ~RobustSolveGuard() { m_backend_->disengage_robust_solve(); }

  RobustSolveGuard(const RobustSolveGuard&) = delete;
  RobustSolveGuard& operator=(const RobustSolveGuard&) = delete;
  RobustSolveGuard(RobustSolveGuard&&) = delete;
  RobustSolveGuard& operator=(RobustSolveGuard&&) = delete;

  /// Layer another × 10 tolerance loosening on top of the current
  /// robust-mode state.  Equivalent to calling
  /// SolverBackend::engage_robust_solve() once more — the saved state
  /// captured by the first engage is preserved so disengage still
  /// restores baseline parameters when the guard expires.
  void escalate() { m_backend_->engage_robust_solve(); }

private:
  SolverBackend* m_backend_;
};

/// Plugin entry-point function type: creates a SolverBackend by solver name.
using solver_backend_factory_fn = SolverBackend* (*)(const char* solver_name);

/// Plugin name function type: returns the plugin name.
using solver_plugin_name_fn = const char* (*)();

/// Plugin solver-list function type: returns null-terminated array of names.
using solver_plugin_names_fn = const char* const* (*)();

/// Plugin ABI version function type: returns the ABI version the plugin was
/// built against.  SolverRegistry rejects plugins whose version differs from
/// k_solver_abi_version.
using solver_plugin_abi_version_fn = int (*)();

/// Plugin infinity function type: returns the solver's representation of
/// +infinity (CPLEX: `CPX_INFBOUND` = 1e20, HiGHS / OSI / Gurobi: 1e30,
/// MindOpt: ~1e30).  Queryable WITHOUT instantiating a `SolverBackend`,
/// so callers like `PlanningLP::auto_scale_*` can compare data values
/// against the solver's infinity threshold to detect "no bound"
/// sentinels (e.g. `Reservoir.fmax = 1e30`) without paying the cost
/// of creating a throwaway backend instance.
///
/// Optional: plugins built before ABI version `k_solver_abi_version`
/// added this symbol may not export it.  `SolverRegistry::load_plugin`
/// treats absence as a soft error and falls back to instance-level
/// query (creates a backend, calls `infinity()`, drops it).
using solver_plugin_infinity_fn = double (*)(const char* solver_name);

}  // namespace gtopt
