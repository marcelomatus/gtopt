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
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/basis.hpp>
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
inline constexpr int k_solver_abi_version = 14;

/// Per-solve effort reported by a backend: wall `seconds` plus deterministic
/// `ticks` (a load-independent work unit).  Conventions for backends lacking
/// one or both (see `SolverBackend::last_solve_effort`):
///   - no native ticks → set `ticks == seconds` (repeat the time);
///   - no native time  → the plugin measures wall time itself.
/// Any backend returning the default `{0,0}` is handled generically by the
/// LinearInterface solve layer, which substitutes its own wall measurement.
struct SolveEffort
{
  double seconds {0.0};
  double ticks {0.0};
};

/// Process-global accumulator of solve effort across every backend solve
/// routed through LinearInterface.  Thread-safe; queryable any time and
/// dumped once at program exit.  Generic across all plugins.
class SolveEffortTotals
{
public:
  static SolveEffortTotals& instance()
  {
    static SolveEffortTotals totals;
    return totals;
  }
  void add(double seconds, double ticks) noexcept
  {
    m_seconds_.fetch_add(seconds, std::memory_order_relaxed);
    m_ticks_.fetch_add(ticks, std::memory_order_relaxed);
    m_count_.fetch_add(1, std::memory_order_relaxed);
  }
  [[nodiscard]] double seconds() const noexcept
  {
    return m_seconds_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] double ticks() const noexcept
  {
    return m_ticks_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] long count() const noexcept
  {
    return m_count_.load(std::memory_order_relaxed);
  }

private:
  SolveEffortTotals() = default;
  ~SolveEffortTotals()
  {
    std::fprintf(
        stderr,
        "GTOPT_SOLVE_EFFORT solver_time=%.3f s ticks=%.1f solves=%ld\n",
        seconds(),
        ticks(),
        count());
  }
  std::atomic<double> m_seconds_ {0.0};
  std::atomic<double> m_ticks_ {0.0};
  std::atomic<long> m_count_ {0};
};

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

  /// Bulk column-bound mutation: parallel arrays of length `num`.
  /// `lu[i]` ∈ {'L','U','B'} per CPLEX convention (lower / upper / both).
  /// Default fallback is a per-element loop over `set_col_lower` /
  /// `set_col_upper`, matching `set_obj_coeffs` shape.  Plugins SHOULD
  /// override when the native API is genuinely batched (CPXchgbds with
  /// `cnt > 1`, HighsLp::col_{lower,upper}, etc.) — those drop the per-
  /// call dispatch overhead and often skip per-element bookkeeping.
  ///
  /// Hot in `LinearInterface::apply_post_load_replay`'s `pending_col_bounds`
  /// loop and the SDDP forward-pass `propagate_trial_values` step.
  virtual void set_col_bounds_bulk(int num,
                                   const int* indices,
                                   const char* lu,
                                   const double* values)
  {
    for (int i = 0; i < num; ++i) {
      switch (lu[i]) {
        case 'L':
          set_col_lower(indices[i], values[i]);
          break;
        case 'U':
          set_col_upper(indices[i], values[i]);
          break;
        case 'B':
          set_col_lower(indices[i], values[i]);
          set_col_upper(indices[i], values[i]);
          break;
        default:
          break;
      }
    }
  }

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
      set_obj_coeff(i, values[i]);
    }
  }

  /// Install a constant additive term on the objective — the solver's
  /// NATIVE objective offset.  Reported objective value becomes
  /// `Σ cⱼ xⱼ + raw_offset` (all backends normalise sign internally so the
  /// term is *added*; CLP, which subtracts its stored offset, negates).
  ///
  /// Semantics: **ABSOLUTE set** in the RAW (scaled) objective units the
  /// backend solves in — the caller pushes the running total, NOT a delta.
  /// `LinearInterface::add_obj_constant` accumulates the total in
  /// `m_scaling_.obj_constant_raw` and calls this with the new absolute value,
  /// so two successive `add_obj_constant` calls end with a single
  /// `set_obj_offset` carrying their sum.
  ///
  /// Why native (not folded post-solve): CPLEX / Gurobi / HiGHS compute the
  /// MIP **relative** optimality gap against the reported objective.  Folding
  /// a large boundary-cut / α-rebase constant only after the solve leaves the
  /// solver's objective near zero, inflating the relative gap into a phantom
  /// 100-170 % that stalls UC MIPs.  Handing the constant to the native offset
  /// makes the solver's gap reflect the true objective.
  ///
  /// Backends store the value so `clone()` preserves it and re-apply it at
  /// load/build (several backends reset the native problem on `load_problem`).
  /// Default is a no-op for ABI/future compatibility, but every shipped plugin
  /// overrides it: `LinearInterface::get_obj_value_raw()` reads the objective
  /// straight from the backend (it no longer re-adds
  /// `m_scaling_.obj_constant_raw`), so a backend that leaves this
  /// unimplemented would silently drop the constant.
  virtual void set_obj_offset(double /*raw_offset*/) noexcept {}

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

  /// Bulk row-bound mutation: parallel arrays of length `num`.
  /// `lu[i]` ∈ {'L','U','B'} per CPLEX convention (lower / upper / both).
  /// Default fallback is a per-element loop over `set_row_lower` /
  /// `set_row_upper` / `set_row_bounds`.  Plugins SHOULD override when
  /// the native API is genuinely batched (CPXchgrhs/CPXchgrngval for
  /// equality and ranged RHS, HighsLp::row_{lower,upper}, etc.).
  ///
  /// Hot in `LinearInterface::update_lp_for_phase` row-RHS recomputation
  /// across many constraints in one phase.
  virtual void set_row_bounds_bulk(int num,
                                   const int* indices,
                                   const char* lu,
                                   const double* lo,
                                   const double* up)
  {
    for (int i = 0; i < num; ++i) {
      switch (lu[i]) {
        case 'L':
          set_row_lower(indices[i], lo[i]);
          break;
        case 'U':
          set_row_upper(indices[i], up[i]);
          break;
        case 'B':
          set_row_bounds(indices[i], lo[i], up[i]);
          break;
        default:
          break;
      }
    }
  }

  // ---- coefficient access ----

  [[nodiscard]] virtual double get_coeff(int row, int col) const = 0;
  virtual void set_coeff(int row, int col, double value) = 0;
  [[nodiscard]] virtual bool supports_set_coeff() const noexcept = 0;

  // ---- variable types ----

  virtual void set_continuous(int index) = 0;
  virtual void set_integer(int index) = 0;
  [[nodiscard]] virtual bool is_continuous(int index) const = 0;
  [[nodiscard]] virtual bool is_integer(int index) const = 0;

  /// Relax every integer column to continuous in a single backend call.
  /// Hot path: invoked once per aperture clone in
  /// `LinearInterface::relax_integers()`, called immediately before the
  /// per-clone backward-pass resolve in
  /// `source/sddp_aperture.cpp` (above `clone.resolve(aperture_opts)`).
  ///
  /// Default fallback enumerates `get_numcols()` and calls
  /// `set_continuous(i)` only on columns where `is_integer(i)` is true,
  /// returning the number of columns flipped.  Plugins SHOULD override
  /// when the native API offers a real one-call path — those overrides
  /// drop both per-call dispatch overhead and any internal per-element
  /// MIP→LP bookkeeping the solver may do.
  ///
  /// @return Number of columns flipped to continuous, or -1 if the
  ///         backend cannot determine the count cheaply (caller logs
  ///         informationally; correctness does not depend on the value).
  virtual int relax_all_integers()
  {
    const int n = get_num_cols();
    int relaxed = 0;
    for (int i = 0; i < n; ++i) {
      if (is_integer(i)) {
        set_continuous(i);
        ++relaxed;
      }
    }
    return relaxed;
  }

  /// Re-mark the given columns as integer, undoing a prior
  /// `relax_all_integers()`.  The inverse operation: callers that relaxed
  /// every integer to solve an LP relaxation in place (MIP-start pipeline)
  /// use this to restore integrality before the real MIP solve.
  ///
  /// `integer_cols` is the snapshot of column indices that were integer
  /// BEFORE relaxation (relaxation hides integrality, so the snapshot must be
  /// taken first).  Default fallback re-calls `set_integer` per column.
  /// Plugins SHOULD override when the native API offers a one-call restore —
  /// CPLEX flips the problem type back to MILP/MIQP, which restores every
  /// column's saved ctype in a single call (the ~10⁵-column per-call loop is
  /// otherwise a real cost on PLEXOS-scale UC MIPs).
  ///
  /// @return Number of columns restored to integer.
  virtual int restore_integers(std::span<const int> integer_cols)
  {
    for (const int c : integer_cols) {
      set_integer(c);
    }
    return static_cast<int>(integer_cols.size());
  }

  /// Fix every integer column to its incumbent (MIP-optimal) value and
  /// re-solve as an LP to recover row duals / reduced costs, warm-started
  /// where the backend supports it.  Called right after a MIP `resolve()`
  /// on the live backend; the fixed LP is a throwaway whose only product
  /// is the committed-solution duals.
  ///
  /// Output is the COMMITTED-solution duals (integers pinned to the
  /// incumbent), identical to the portable default path — only wall-clock
  /// differs.
  ///
  /// Default (portable) path: gather integer columns via `is_integer`,
  /// read the current column solution (`col_solution`), pin each integer
  /// column to `std::round(value)` on both bounds, `relax_all_integers`,
  /// `apply_options(opts)`, then `resolve()`.  This default is already
  /// warm on Clp/OSI (its `resolve()` retains the basis) and correct
  /// everywhere.  CPLEX overrides with the single-call `CPXPROB_FIXEDMILP`
  /// fast path (fixes integers AND installs the incumbent-node basis, then
  /// a warm dual-simplex re-solve — no barrier, no crossover).
  ///
  /// @param opts Solver options for the follow-up LP solve (the caller
  ///             typically enables `crossover` so a barrier backend
  ///             produces clean vertex duals).
  /// @return Number of fixed integer columns: `0` = pure LP / nothing to
  ///         do (caller's existing duals already valid); `> 0` = fixed and
  ///         re-solved (caller must refresh its solution cache from the
  ///         backend); `< 0` = no incumbent / failure (caller keeps the
  ///         MIP result untouched).
  virtual int fix_mip_and_resolve_duals(const SolverOptions& opts)
  {
    const int n = get_num_cols();
    // Read the current (MIP-optimal) primal BEFORE any bound mutation —
    // the first `set_col_lower/upper` may invalidate the solution view.
    const double* sol = col_solution();
    if (sol == nullptr) {
      return -1;  // no incumbent solution available
    }

    int fixed = 0;
    for (int i = 0; i < n; ++i) {
      if (is_integer(i)) {
        // Round to the nearest integer: an integer column may report a
        // value a hair off the lattice (e.g. 0.9999997) under tolerance.
        const double pinned = std::round(sol[i]);
        set_col_lower(i, pinned);
        set_col_upper(i, pinned);
        ++fixed;
      }
    }
    if (fixed == 0) {
      return 0;  // pure LP — caller's duals are already valid
    }

    relax_all_integers();
    apply_options(opts);
    resolve();
    return fixed;
  }

  // ---- SOS (special-ordered set) constraints ----

  /// Declare a Type-2 Special-Ordered-Set (SOS2) over the given column
  /// indices.  SOS2 enforces that at most TWO of the listed columns
  /// can be non-zero in any feasible MIP solution, and that the two
  /// non-zero columns must be adjacent in the listed order.  This is
  /// the canonical MIP encoding of piecewise-linear functions over a
  /// convex partition (Beale & Tomlin 1970; Vielma 2015 §3).
  ///
  /// In gtopt this is consumed by the ``tangent_signed_flow``
  /// L-secant upper bound (issue #504): with ``L > 1`` segment
  /// columns ``v_l`` and ``Σ v_l ≥ |f|``, the SOS2 enforces
  /// fill-order (v_1 saturates before v_2 starts) so the piecewise
  /// chord becomes tight at every breakpoint.
  ///
  /// Index ordering in ``columns`` MUST match the geometric order of
  /// the underlying breakpoints.  Weights default to ``1, 2, 3, …, N``
  /// in the backend implementation — gtopt does NOT expose custom
  /// SOS2 weights since the segment widths are already encoded in
  /// the column upper bounds.
  ///
  /// Backend support matrix:
  ///   * CPLEX    — overrides with ``CPXaddsos`` (sostype 2)
  ///   * Gurobi   — overrides with ``GRBaddsos`` (GRB_SOS_TYPE2)
  ///   * HiGHS    — overrides with ``Highs_addSos`` when linked
  ///                against HiGHS ≥ 1.6 (older versions: default
  ///                fallback fires)
  ///   * CBC/OSI  — no SOS2 entry point on the OSI bridge gtopt
  ///                uses; default fallback fires
  ///   * MindOpt  — TBD; default fallback fires
  ///
  /// Default implementation throws ``std::runtime_error`` so a
  /// misconfigured LP (``loss_use_sos2 = true`` against an
  /// unsupporting plugin) surfaces at LP-build time rather than
  /// silently dropping the SOS2 declaration.  Backends that DO
  /// support SOS2 override this method.
  ///
  /// @param columns Column indices, ordered to match the geometric
  ///                breakpoint sequence (`v_1, v_2, …, v_L`).
  ///                Must be non-empty; size < 2 is a no-op
  ///                (SOS2 over a single column is vacuous).
  virtual void add_sos2(std::span<const int> columns)
  {
    if (columns.size() < 2) {
      return;
    }
    throw std::runtime_error(
        std::format("SolverBackend '{}': add_sos2 not implemented.  "
                    "Issue gtopt#504 needs a MIP backend with native "
                    "SOS2 (CPLEX / Gurobi / HiGHS ≥ 1.6); the current "
                    "backend cannot enforce SOS2 over {} columns.  "
                    "Either switch solver via --solver, or disable "
                    "loss_use_sos2 on the affected line(s).",
                    solver_name(),
                    columns.size()));
  }

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
  // `post_solve` in compress mode so the LI's `m_cached_*` is the
  // sole destination — no plugin scratch buffer is touched.
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
  // Span-out variants for column-bound vectors.  Mirrors fill_col_sol /
  // fill_col_cost / fill_row_dual but for the construction-time bounds
  // that `LinearInterface::populate_solution_cache_post_solve` snapshots
  // into `m_cache_.col_low()` / `m_cache_.col_upp()` post-solve under
  // compress mode.  Without these, the LI's
  // `get_col_low_raw` / `get_col_upp_raw` always called
  // `backend().col_lower() / col_upper()` directly — forcing every
  // CPLEX/MindOpt/Gurobi backend to allocate a `numcols`-sized
  // `m_collb_` / `m_colub_` scratch on every read.  Default impl
  // memcpy's from the matching pointer-getter (OSI/HiGHS); CPLEX
  // overrides with `CPXgetlb` / `CPXgetub` directly into the caller's
  // buffer for the same memory invariant the solution fillers already
  // satisfy.
  virtual void fill_col_lower(std::span<double> out) const
  {
    if (const auto* p = col_lower(); p != nullptr) {
      std::copy_n(p, out.size(), out.data());
    }
  }
  virtual void fill_col_upper(std::span<double> out) const
  {
    if (const auto* p = col_upper(); p != nullptr) {
      std::copy_n(p, out.size(), out.data());
    }
  }

  // ---- solution hints (warm start) ----

  virtual void set_col_solution(const double* sol) = 0;
  virtual void set_row_price(const double* price) = 0;

  /// Inject a starting integer (MIP-start / warm incumbent) solution.
  ///
  /// Unlike `set_col_solution` (an LP-basis/primal warm start that branch-and-
  /// cut discards), this installs a *named MIP start* the solver seeds B&C
  /// with — the mechanism that lets a good external commitment bypass a
  /// solver's costly node-0 heuristic incumbent.
  ///
  /// `col_values` is a DENSE vector in RAW LP column space (length ==
  /// `get_num_cols()`), the inverse of `col_solution()` — callers building it
  /// from a LinearInterface MUST use the raw (un-descaled) solution accessor.
  /// `effort` selects how hard the backend validates / completes / repairs the
  /// start (see `MipStartEffort`).
  ///
  /// Backend support:
  ///   * CPLEX   — `CPXaddmipstarts` with the matching `CPX_MIPSTART_*` level.
  ///   * Gurobi / MindOpt / HiGHS — their native dense `Start` / `col_value`
  ///     attribute (effort is best-effort or ignored).
  ///   * OSI/CBC, cuOpt — not wired; default below.
  ///
  /// Default implementation is a benign no-op returning `false` (LP-only or
  /// unsupporting backends): a missing MIP start is a skipped optimisation,
  /// not an error (contrast `add_sos2`, which is load-time-fatal).
  ///
  /// @param col_values Dense raw-space column values (size == num cols).
  /// @param effort     Backend validation/completion effort for the start.
  /// @return `true` if the backend accepted/installed the start, else `false`.
  virtual bool set_mip_start(std::span<const double> /*col_values*/,
                             MipStartEffort /*effort*/)
  {
    return false;
  }

  // ---- simplex basis (advanced warm start) ----

  /// Capture the current simplex basis (column + row statuses).
  ///
  /// Returns `std::nullopt` when no basis is available — the LP was not
  /// solved by simplex/crossover (a bare interior point has no basis), or
  /// the backend is basis-less (cuOpt PDLP).  On success the result has
  /// `col_status.size() == get_num_cols()` and
  /// `row_status.size() == get_num_rows()`.
  ///
  /// Default: unsupported (`nullopt`).  A basis is an `(n+m)` status vector
  /// — no matrix data, no solution values.
  [[nodiscard]] virtual std::optional<Basis> get_basis() const
  {
    return std::nullopt;
  }

  /// Install a simplex basis as an advanced start for the next solve.
  ///
  /// PRECONDITION: `basis` is sized to the CURRENT problem
  /// (`col_status.size() == get_num_cols()`,
  /// `row_status.size() == get_num_rows()`).  `LinearInterface::set_basis`
  /// reconciles a basis captured from a related LP (cuts appended rows)
  /// to these dimensions via `reconcile_basis` before calling here, so the
  /// backend only maps statuses to native and installs them; a size
  /// mismatch is rejected (returns `false`) rather than guessed.
  ///
  /// Pairs with `SolverOptions::advanced_basis` (→ CPLEX `ADVIND=1`) and
  /// `LPAlgo::dual` so dual simplex resumes from the seed.
  ///
  /// Default: unsupported (`false`) — a benign skip, like `set_mip_start`.
  virtual bool set_basis(const Basis& /*basis*/) { return false; }

  // ---- infeasibility diagnosis ----

  /// Diagnose why the current problem is infeasible, returning a human-readable
  /// list of the conflicting constraints (a minimal infeasible subsystem).
  /// Used by the MIP-start relaxation pre-pass when the LP relaxation comes
  /// back infeasible and the user requested `feasopt` diagnosis.
  ///
  /// Backends: CPLEX overrides via the conflict refiner (`CPXrefineconflict` +
  /// `CPXgetconflict`).  Others return `std::nullopt` (diagnosis unsupported);
  /// the caller then reports only that the relaxation is infeasible.
  ///
  /// @param max_items Cap on the number of constraint labels returned.
  /// @return Conflicting constraint labels, or `std::nullopt` if unsupported.
  virtual std::optional<std::vector<std::string>> diagnose_infeasibility(
      int /*max_items*/)
  {
    return std::nullopt;
  }

  // ---- solve ----

  virtual void initial_solve() = 0;
  virtual void resolve() = 0;

  /// Report the most recent solve's effort (wall seconds + deterministic
  /// ticks).  Default `{0,0}` means "not provided" — the LinearInterface
  /// solve layer then substitutes its own measured wall time (and
  /// `ticks = seconds`).  Backends with native instrumentation override:
  /// CPLEX uses `CPXgettime` + `CPXgetdettime`; backends without ticks set
  /// `ticks == seconds`; backends without native time measure it themselves.
  [[nodiscard]] virtual SolveEffort last_solve_effort() const { return {}; }

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
