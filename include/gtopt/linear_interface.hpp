/**
 * @file      linear_interface.hpp
 * @brief     Interface to linear programming solvers
 * @date      Mon Mar 24 09:41:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a unified interface to various linear programming
 * solvers through a pluggable SolverBackend abstraction.  It enables
 * problem construction, solving, and solution retrieval in a solver-agnostic
 * manner, simplifying the integration of different planning engines.
 *
 * Solver backends (CLP, CBC, CPLEX, HiGHS, …) are loaded as dynamic
 * plugins at runtime via the SolverRegistry.
 */

#pragma once

#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>

#include <gtopt/error.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/strong_index_vector.hpp>

namespace gtopt
{

/// Zero-copy lazy view that applies a per-element scale factor.
///
/// Models a random-access range: `view[i]` returns `data[i] * scales[i]`
/// (or `data[i] / scales[i]` when constructed with `divides`).
/// When scales are empty, returns raw data unchanged.
///
/// Accepts any integer-like index type (int, size_t, ColIndex, RowIndex).
class ScaledView
{
public:
  enum class Op : uint8_t
  {
    multiply,
    divide,
  };

  constexpr ScaledView() noexcept = default;

  constexpr ScaledView(const double* data,
                       size_t n,
                       const double* scales,
                       size_t ns,
                       Op op = Op::multiply,
                       double global_factor = 1.0) noexcept
      : data_(data, n)
      , scales_(scales, ns)
      , op_(op)
      , global_(global_factor)
  {
  }

  /// Construct from a raw span (no scaling).
  constexpr explicit ScaledView(std::span<const double> raw) noexcept
      : data_(raw)
  {
  }

  [[nodiscard]] constexpr double operator[](auto idx) const noexcept
  {
    const auto i = static_cast<size_t>(idx);
    if (i >= scales_.size()) {
      return data_[i] * global_;
    }
    const auto v =
        (op_ == Op::multiply) ? data_[i] * scales_[i] : data_[i] / scales_[i];
    return v * global_;
  }

  [[nodiscard]] constexpr size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return data_.empty(); }

  /// Iterator support for range-for loops.
  class iterator
  {
  public:
    using value_type = double;
    using difference_type = ptrdiff_t;

    constexpr iterator() noexcept = default;
    constexpr iterator(const ScaledView* view, size_t pos) noexcept
        : view_(view)
        , pos_(pos)
    {
    }

    constexpr double operator*() const noexcept { return (*view_)[pos_]; }
    constexpr iterator& operator++() noexcept
    {
      ++pos_;
      return *this;
    }
    constexpr iterator operator++(int) noexcept
    {
      auto tmp = *this;
      ++pos_;
      return tmp;
    }
    constexpr bool operator==(const iterator& o) const noexcept = default;

  private:
    const ScaledView* view_ {};
    size_t pos_ {};
  };

  [[nodiscard]] constexpr iterator begin() const noexcept { return {this, 0}; }
  [[nodiscard]] constexpr iterator end() const noexcept
  {
    return {this, data_.size()};
  }

private:
  std::span<const double> data_ {};
  std::span<const double> scales_ {};
  Op op_ {Op::multiply};
  double global_ {1.0};  ///< Uniform factor applied to every element
};

/// Diagnostics for a single LP row (constraint or cut).
/// Used by kappa_warning=diagnose to identify ill-conditioned rows.
struct RowDiagnostics
{
  RowIndex row {};
  std::string name {};
  double rhs_lb {};
  double rhs_ub {};
  double min_abs_coeff {std::numeric_limits<double>::max()};
  double max_abs_coeff {};
  double coeff_ratio {1.0};
  int num_nonzeros {};
  std::string min_col_name {};
  std::string max_col_name {};
};

class LinearInterface
{
public:
  /** @brief Copy constructor disabled */
  explicit LinearInterface(const LinearInterface&) = delete;
  /** @brief Copy assignment disabled */
  LinearInterface& operator=(const LinearInterface&) = delete;
  /** @brief Move constructor */
  LinearInterface(LinearInterface&&) = default;
  /** @brief Move assignment */
  LinearInterface& operator=(LinearInterface&&) = default;

  /**
   * @brief Constructs interface with a solver backend by name
   * @param solver_name Solver identifier ("clp", "cbc", "cplex", "highs")
   * @param plog_file Path to log file for solver output
   */
  explicit LinearInterface(std::string_view solver_name = {},
                           const std::string& plog_file = {});

  /**
   * @brief Constructs interface with a pre-created backend
   * @param backend Pre-configured solver backend
   * @param plog_file Path to log file for solver output
   */
  explicit LinearInterface(std::unique_ptr<SolverBackend> backend,
                           std::string plog_file = {});

  /**
   * @brief Constructs interface, loads a problem, with solver by name
   * @param solver_name Solver identifier
   * @param flat_lp Flattened linear problem to load
   * @param plog_file Path to log file for solver output
   */
  LinearInterface(std::string_view solver_name,
                  const FlatLinearProblem& flat_lp,
                  const std::string& plog_file = {});

  ~LinearInterface() = default;

  /**
   * @brief Creates a deep copy of this LinearInterface via the backend's
   *        clone() method.
   *
   * The clone preserves the full LP state (variables, constraints, bounds,
   * objective, warm-start basis).  Modifications to the clone do not affect
   * the original — use this for tentative solves such as the SDDP elastic
   * filter, where the original LP must remain unmodified.
   *
   * @param col_sol  Optional primal solution for warm-start (empty = skip)
   * @param row_dual Optional dual solution for warm-start (empty = skip)
   * @return A new LinearInterface wrapping the cloned solver.
   */
  [[nodiscard]] LinearInterface clone(
      std::span<const double> col_sol = {},
      std::span<const double> row_dual = {}) const;

  /**
   * @brief Apply a saved solution as warm-start hint for the next resolve.
   *
   * Handles dimension mismatches gracefully: if the LP has gained extra
   * columns (e.g. elastic slack variables) or extra rows (e.g. Benders cuts)
   * since the solution was captured, the vectors are zero-padded to match.
   * If the saved vector is larger than the current LP dimension it is
   * silently ignored (stale snapshot).
   *
   * @param col_sol  Primal solution to set (empty = skip)
   * @param row_dual Dual solution to set (empty = skip)
   */
  void set_warm_start_solution(std::span<const double> col_sol,
                               std::span<const double> row_dual);

  /**
   * @brief Release the solver backend, freeing its memory.
   *
   * All LinearInterface metadata (scales, name maps, variable_scale_map,
   * base_numrows, stats) is preserved.  When low_memory_level >= 2, the
   * saved flat LP numeric vectors are compressed before release.
   * The backend can be recreated by a subsequent call to
   * reconstruct_backend() or load_flat().
   */
  void release_backend() noexcept;

  /**
   * @brief Check whether the solver backend is loaded.
   * @return true if load_flat() has been called and release_backend() has not
   */
  [[nodiscard]] bool has_backend() const noexcept
  {
    return m_backend_ != nullptr;
  }

  // ── Low-memory mode API ─────────────────────────────────────────────────

  /// Configure low-memory mode (off, snapshot, compress).
  /// @param cache_warm_start  When true (default), release_backend() caches
  ///   the full primal/dual solution so that ensure_backend() can warm-start
  ///   the reconstructed LP.  When false, the solution vectors are discarded
  ///   on release to save memory — callers must supply their own warm-start
  ///   data via reconstruct_backend(col_sol, row_dual).
  void set_low_memory(LowMemoryMode mode,
                      CompressionCodec codec = CompressionCodec::lz4,
                      bool cache_warm_start = false) noexcept;

  /// Save a flat LP snapshot for future reconstruction.
  /// Call from create_lp() — the LinearInterface decides whether to keep
  /// based on the configured level.
  void save_snapshot(FlatLinearProblem flat_lp);

  /// Install a flat LP snapshot **without** loading the backend.
  ///
  /// Used by `SystemLP::create_lp` when low-memory mode is enabled at
  /// build time so the initial `load_flat()` call can be skipped: the
  /// snapshot is recorded, optionally compressed (level `compress`), and
  /// `m_backend_released_` is flipped on so the next user-driven
  /// `add_col` / `add_row` / solve goes through `ensure_backend()` →
  /// `reconstruct_backend()` (which performs the single
  /// build-time `load_flat`).
  ///
  /// Pre-condition: `set_low_memory()` must have been called first with a
  /// non-`off` mode — otherwise this would defeat the lazy reconstruction
  /// path because `release_backend()` becomes a no-op.
  void defer_initial_load(FlatLinearProblem flat_lp);

  /// Capture hot-start cuts (rows above base_numrows) into active_cuts.
  /// Call after all initialization (alpha vars, hot-start cuts) is done.
  void capture_hot_start_cuts();

  /// Reconstruct backend from saved flat LP + dynamic cols + active cuts.
  /// @param col_sol  Previous primal solution for warm-start (empty = cold)
  /// @param row_dual Previous dual solution for warm-start (empty = cold)
  void reconstruct_backend(std::span<const double> col_sol = {},
                           std::span<const double> row_dual = {});

  /// True if the solver backend has been released (low-memory mode).
  [[nodiscard]] bool is_backend_released() const noexcept
  {
    return m_backend_released_;
  }

  /// Current low-memory mode.
  [[nodiscard]] constexpr LowMemoryMode low_memory_mode() const noexcept
  {
    return m_low_memory_mode_;
  }

  /// Record a dynamically-added column (e.g. alpha variable).
  void record_dynamic_col(SparseCol col);

  /// Record a Benders cut row addition.
  void record_cut_row(SparseRow row);

  /// Record cut row deletions (pruning/forgetting).
  void record_cut_deletion(std::span<const int> deleted_indices);

  /// Decompress the saved flat LP (level 2) and keep it uncompressed
  /// until enable_compression() is called.  No-op if level < 2 or
  /// already decompressed.
  void disable_compression();

  /// Re-compress the saved flat LP (level 2).  Reverses a prior
  /// disable_compression().  No-op if level < 2 or already compressed.
  void enable_compression();

  /**
   * @brief Loads a flattened linear problem into the solver
   *
   * If the backend was previously released, it is automatically recreated
   * from the saved solver name before loading.
   *
   * @param flat_lp The flattened problem representation
   * @throws std::runtime_error if the problem cannot be loaded
   */
  void load_flat(const FlatLinearProblem& flat_lp);

  /**
   * @brief Adds a new column from a SparseCol with metadata-based naming.
   *
   * Generates the column name lazily from structured metadata when
   * class_name and context are present; returns empty otherwise.
   * Applies bounds, objective coefficient, and scale.
   *
   * @param col The sparse column specification
   * @return The index of the newly added column
   */
  ColIndex add_col(const SparseCol& col);

  /// Bulk-add columns (calls add_col for each).
  void add_cols(std::span<const SparseCol> cols);

  /**
   * @brief Adds a new constraint row to the problem
   * @param row The sparse row representation of the constraint
   * @param eps Epsilon value for coefficient filtering (values below this are
   * ignored)
   * @return The index of the newly added row
   */
  RowIndex add_row(const SparseRow& row, double eps = 0.0);

  /**
   * @brief Bulk-add constraint rows (much faster than repeated add_row calls).
   *
   * Converts SparseRows to CSR format, applies scaling and bound
   * normalization, dispatches a single add_rows() call to the solver backend,
   * and updates row scales and name maps.
   *
   * @param rows The sparse row representations of the constraints
   * @param eps  Epsilon value for coefficient filtering
   */
  void add_rows(std::span<const SparseRow> rows, double eps = 0.0);

  /**
   * @brief Deletes rows by index from the constraint matrix.
   *
   * @param indices  Row indices to delete (must be sorted ascending)
   */
  void delete_rows(std::span<const int> indices);

  /**
   * @brief Save the current row count as the "base" model size.
   *
   * All rows added after this point are considered cuts and are eligible
   * for pruning.  Must be called once after the structural LP is built,
   * before any Benders cuts are added.
   */
  void save_base_numrows() noexcept { m_base_numrows_ = get_numrows(); }

  /**
   * @brief Get the saved base row count.
   * @return Base row count (0 if save_base_numrows was never called)
   */
  [[nodiscard]] constexpr size_t base_numrows() const noexcept
  {
    return m_base_numrows_;
  }

  /**
   * @brief Reset this LP to a base state by copying column bounds from
   *        @p source and deleting any rows beyond @p base_rows.
   *
   * Used by the clone pool to reuse a cached LP clone across aperture
   * solves without re-allocating the underlying solver.
   *
   * @param source    The original (unmodified) LP to copy bounds from
   * @param base_rows Number of structural rows to keep (delete beyond)
   */
  void reset_from(const LinearInterface& source, size_t base_rows);

  /**
   * @brief Gets the number of constraint rows in the problem
   * @return Number of rows
   */
  [[nodiscard]] size_t get_numrows() const;

  /**
   * @brief Gets the number of variable columns in the problem
   * @return Number of columns
   */
  [[nodiscard]] size_t get_numcols() const;

  /// Solver backend name (e.g. "clp", "cplex", "highs").
  [[nodiscard]] std::string_view solver_name() const noexcept
  {
    return m_solver_name_;
  }

  /// Solver library version string (e.g. "1.17.3").
  /// Safe to call even when the backend has been released.
  [[nodiscard]] std::string solver_version() const
  {
    if (m_backend_) {
      return m_backend_->solver_version();
    }
    return m_solver_version_;
  }

  /// Solver identifier: "name/version" (e.g. "highs/1.13.1").
  /// Safe to call even when the backend has been released.
  [[nodiscard]] std::string solver_id() const
  {
    return std::format("{}/{}", solver_name(), solver_version());
  }

  /// Currently configured LP algorithm.
  [[nodiscard]] LPAlgo get_algorithm() const
  {
    return m_backend_->get_algorithm();
  }

  /// Currently configured thread count (0 = solver default).
  [[nodiscard]] int get_threads() const { return m_backend_->get_threads(); }

  /// Whether presolve is currently enabled.
  [[nodiscard]] bool get_presolve() const { return m_backend_->get_presolve(); }

  /// Current solver log verbosity level (0 = off).
  [[nodiscard]] int get_log_level() const
  {
    return m_backend_->get_log_level();
  }

  /// Solver's representation of +infinity for variable bounds.
  [[nodiscard]] double infinity() const noexcept
  {
    return m_backend_->infinity();
  }

  /// Normalize a bound value: map gtopt::DblMax to the solver's infinity.
  ///
  /// SparseCol/SparseRow use `std::numeric_limits<double>::max()` as default
  /// unbounded markers, but solver backends (e.g. HiGHS) may use a smaller
  /// infinity threshold (e.g. 1e30).  This method translates at the
  /// LinearInterface boundary so that formulation code and solver agree.
  [[nodiscard]] double normalize_bound(double value) const noexcept
  {
    const auto inf = m_backend_->infinity();
    if (value >= DblMax) {
      return inf;
    }
    if (value <= -DblMax) {
      return -inf;
    }
    return value;
  }

  /// Normalize a (lower, upper) bound pair in one call.
  [[nodiscard]] std::pair<double, double> normalize_bounds(
      double lowb, double uppb) const noexcept
  {
    return {normalize_bound(lowb), normalize_bound(uppb)};
  }

  /// True if @p value represents positive infinity for the active solver.
  [[nodiscard]] bool is_pos_inf(double value) const noexcept
  {
    return value >= m_backend_->infinity();
  }

  /// True if @p value represents negative infinity for the active solver.
  [[nodiscard]] bool is_neg_inf(double value) const noexcept
  {
    return value <= -m_backend_->infinity();
  }

  // ── Row bound setters (raw: LP/solver units) ──

  void set_rhs_raw(RowIndex row, double rhs);
  void set_row_low_raw(RowIndex index, double value);
  void set_row_upp_raw(RowIndex index, double value);

  // ── Row bound setters (physical: descaled) ──

  void set_rhs(RowIndex row, double physical_rhs);
  void set_row_low(RowIndex index, double physical_value);
  void set_row_upp(RowIndex index, double physical_value);

  // ── Coefficient accessors (raw: LP/solver units) ──

  /**
   * @brief Gets a raw coefficient from the constraint matrix (LP units).
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @return The raw coefficient value, or 0.0 if the element does not exist
   */
  [[nodiscard]] double get_coeff_raw(RowIndex row, ColIndex column) const;

  /**
   * @brief Sets (modifies) a raw coefficient in the constraint matrix.
   *
   * The value is stored as-is in LP units.
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @param value Raw coefficient value (LP units)
   */
  void set_coeff_raw(RowIndex row, ColIndex column, double value);

  // ── Coefficient accessors (physical: descaled) ──

  /**
   * @brief Gets a physical coefficient from the constraint matrix.
   *
   * Converts from LP to physical units:
   *   physical_coeff = raw_coeff × col_scale × row_scale
   *
   * When no scaling is active, returns the raw value unchanged.
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @return The physical coefficient value
   */
  [[nodiscard]] double get_coeff(RowIndex row, ColIndex column) const;

  /**
   * @brief Sets a coefficient in the constraint matrix from physical units.
   *
   * Converts from physical to LP units:
   *   raw_coeff = physical_coeff / col_scale / row_scale
   *
   * @param row Row index of the coefficient
   * @param column Column index of the coefficient
   * @param physical_value Physical coefficient value
   */
  void set_coeff(RowIndex row, ColIndex column, double physical_value);

  /**
   * @brief Checks whether the solver supports in-place coefficient updates
   *
   * @return true if set_coeff() is functional
   */
  [[nodiscard]] bool supports_set_coeff() const noexcept;

  void set_obj_coeff(ColIndex index, double value);

  [[nodiscard]] auto get_obj_coeff() const
  {
    return std::span(m_backend_->obj_coefficients(), get_numcols());
  }

  // ── Column bound setters (raw: LP/solver units) ──

  void set_col_low_raw(ColIndex index, double value);
  void set_col_upp_raw(ColIndex index, double value);
  void set_col_raw(ColIndex index, double value);

  // ── Column bound setters (physical: descaled) ──

  void set_col_low(ColIndex index, double physical_value);
  void set_col_upp(ColIndex index, double physical_value);
  void set_col(ColIndex index, double physical_value);

  [[nodiscard]] double get_obj_value() const;

  /**
   * @brief Gets the objective value in physical (unscaled) units.
   *
   * Returns `raw_obj × scale_objective`, converting from LP space back
   * to physical cost units.  Equivalent to the old manual descaling
   * `get_obj_value() * options.scale_objective()`.
   * @return Physical objective value
   */
  [[nodiscard]] double get_obj_value_physical() const;

  /**
   * @brief Writes the problem to an LP format file
   * @param filename Name of the file to write (without extension)
   * @return Success, or an error if row names are not available
   */
  [[nodiscard]] std::expected<void, Error> write_lp(
      const std::string& filename) const;

  /**
   * @brief Performs initial solve of the problem from scratch
   * @param solver_options Options controlling the solve process
   * @return Expected with solver status code (0 = optimal) or error
   */
  [[nodiscard]] std::expected<int, Error> initial_solve(
      const SolverOptions& solver_options = {});

  /**
   * @brief Resolves the problem with updated data using warm start
   * @param solver_options Options controlling the solve process
   * @return Expected with solver status code (0 = optimal) or error
   */
  [[nodiscard]] std::expected<int, Error> resolve(
      const SolverOptions& solver_options = {});

  /**
   * @brief Gets the condition number of the basis matrix (if available)
   * @return Condition number kappa, or -1.0 if not supported by backend
   */
  [[nodiscard]] double get_kappa() const;

  /**
   * @brief Analyze coefficient range for a single row.
   *
   * Iterates over all columns to find non-zero coefficients and reports
   * min/max absolute values, their ratio, and associated column names.
   * Used by kappa diagnostics to identify ill-conditioned cut rows.
   *
   * @param row  Row index to analyze
   * @return RowDiagnostics with coefficient statistics
   */
  [[nodiscard]] RowDiagnostics diagnose_row(RowIndex row) const;

  /**
   * @brief Gets the solver-specific status code
   * @return Status code (interpretation depends on solver)
   */
  [[nodiscard]] int get_status() const;

  /**
   * @brief Checks if the solution is optimal
   * @return True if optimal solution found, false otherwise
   */
  [[nodiscard]] bool is_optimal() const;

  /**
   * @brief Checks if the problem is dual infeasible
   * @return True if dual infeasible, false otherwise
   */
  [[nodiscard]] bool is_dual_infeasible() const;

  /**
   * @brief Checks if the problem is primal infeasible
   * @return True if primal infeasible, false otherwise
   */
  [[nodiscard]] bool is_prim_infeasible() const;

  /**
   * @brief Sets a variable to be continuous (floating-point)
   * @param index Column index to modify
   */
  void set_continuous(ColIndex index);

  /**
   * @brief Sets a variable to be integer
   * @param index Column index to modify
   */
  void set_integer(ColIndex index);

  /**
   * @brief Sets a variable to be binary (0-1 integer)
   * @param index Column index to modify
   */
  void set_binary(ColIndex index);

  /**
   * @brief Checks if a variable is continuous
   * @param index Column index to check
   * @return True if continuous, false otherwise
   */
  [[nodiscard]] bool is_continuous(ColIndex index) const;

  /**
   * @brief Checks if a variable is integer
   * @param index Column index to check
   * @return True if integer, false otherwise
   */
  [[nodiscard]] bool is_integer(ColIndex index) const;

  /**
   * @brief Sets a time limit for the solver
   * @param time_limit Maximum solve time in seconds
   */
  void set_time_limit(double time_limit);

  // ── Row bound getters (raw: LP/solver units) ──

  /**
   * @brief Gets raw lower bounds for all constraint rows (LP units).
   *
   * When row equilibration is active, these are the equilibrated bounds
   * (divided by the per-row scale factor).  Use get_row_low() for
   * physical (unscaled) bounds.
   * @return Span view of raw row lower bounds
   */
  [[nodiscard]] auto get_row_low_raw() const
  {
    return std::span(m_backend_->row_lower(), get_numrows());
  }

  /**
   * @brief Gets raw upper bounds for all constraint rows (LP units).
   * @return Span view of raw row upper bounds
   */
  [[nodiscard]] auto get_row_upp_raw() const
  {
    return std::span(m_backend_->row_upper(), get_numrows());
  }

  // ── Row bound getters (physical: descaled) ──

  /**
   * @brief Gets physical lower bounds for all constraint rows.
   *
   * When row equilibration is active, the raw bounds are multiplied
   * by the per-row scale factor to recover physical units:
   * physical_bound = LP_bound × row_scale.
   * When row scales are empty, returns raw values unchanged.
   * @return ScaledView over solver row lower bounds
   */
  [[nodiscard]] ScaledView get_row_low() const noexcept
  {
    const auto n = get_numrows();
    return {m_backend_->row_lower(),
            n,
            m_row_scales_.data(),
            m_row_scales_.size(),
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets physical upper bounds for all constraint rows.
   * @return ScaledView over solver row upper bounds
   */
  [[nodiscard]] ScaledView get_row_upp() const noexcept
  {
    const auto n = get_numrows();
    return {m_backend_->row_upper(),
            n,
            m_row_scales_.data(),
            m_row_scales_.size(),
            ScaledView::Op::multiply};
  }

  // ── Column bound getters (raw: LP/solver units) ──

  /**
   * @brief Gets raw lower bounds for all variable columns (LP units).
   *
   * These are the bounds as stored in the solver backend.  For scaled
   * variables, LP_bound = physical_bound / col_scale.  Use
   * get_col_low() for physical (unscaled) bounds.
   * @return Span view of raw column lower bounds
   */
  [[nodiscard]] auto get_col_low_raw() const
  {
    return std::span(m_backend_->col_lower(), get_numcols());
  }

  /**
   * @brief Gets raw upper bounds for all variable columns (LP units).
   * @return Span view of raw column upper bounds
   */
  [[nodiscard]] auto get_col_upp_raw() const
  {
    return std::span(m_backend_->col_upper(), get_numcols());
  }

  // ── Column bound getters (physical: descaled) ──

  /**
   * @brief Gets physical lower bounds for all variable columns.
   *
   * Returns a zero-copy lazy view: each access computes
   * `LP_bound × col_scale` on the fly.  When col_scales are empty,
   * returns raw values unchanged.
   * @return ScaledView over solver column lower bounds
   */
  [[nodiscard]] ScaledView get_col_low() const noexcept
  {
    const auto n = get_numcols();
    return {m_backend_->col_lower(),
            n,
            m_col_scales_.data(),
            m_col_scales_.size(),
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets physical upper bounds for all variable columns.
   *
   * Returns a zero-copy lazy view: each access computes
   * `LP_bound × col_scale` on the fly.
   * @return ScaledView over solver column upper bounds
   */
  [[nodiscard]] ScaledView get_col_upp() const noexcept
  {
    const auto n = get_numcols();
    return {m_backend_->col_upper(),
            n,
            m_col_scales_.data(),
            m_col_scales_.size(),
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets raw LP solution values (no descaling).
   *
   * Returns solver values as-is, in LP units.  Use this for SDDP
   * state propagation, cut generation, and any internal LP algebra
   * that operates in scaled coordinates.
   * @return Span view of raw LP solution values
   */
  [[nodiscard]] auto get_col_sol_raw() const -> std::span<const double>
  {
    if (m_backend_released_) {
      return m_cached_col_sol_;
    }
    return {m_backend_->col_solution(), get_numcols()};
  }

  /**
   * @brief Gets physical solution values (LP × col_scale).
   *
   * Returns a zero-copy lazy view: each access computes
   * `LP_value × col_scale` on the fly.  When col_scales are empty,
   * returns raw values unchanged.
   * @return ScaledView over solver solution memory
   */
  [[nodiscard]] ScaledView get_col_sol() const noexcept
  {
    if (m_backend_released_) {
      return {m_cached_col_sol_.data(),
              m_cached_col_sol_.size(),
              m_col_scales_.data(),
              m_col_scales_.size(),
              ScaledView::Op::multiply};
    }
    const auto n = get_numcols();
    return {m_backend_->col_solution(),
            n,
            m_col_scales_.data(),
            m_col_scales_.size(),
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets raw LP reduced costs (no descaling).
   * @return Span view of raw LP reduced costs
   */
  [[nodiscard]] auto get_col_cost_raw() const -> std::span<const double>
  {
    if (m_backend_released_) {
      return m_cached_col_cost_;
    }
    return {m_backend_->reduced_cost(), get_numcols()};
  }

  /**
   * @brief Gets physical reduced costs (LP × scale_objective / col_scale).
   *
   * Returns a zero-copy lazy view: each access computes
   * `LP_rc × scale_objective / col_scale` on the fly, recovering the
   * physical reduced cost in $/physical_unit.
   * @return ScaledView over solver reduced-cost memory
   */
  [[nodiscard]] ScaledView get_col_cost() const noexcept
  {
    if (m_backend_released_) {
      return {m_cached_col_cost_.data(),
              m_cached_col_cost_.size(),
              m_col_scales_.data(),
              m_col_scales_.size(),
              ScaledView::Op::divide,
              m_scale_objective_};
    }
    const auto n = get_numcols();
    return {m_backend_->reduced_cost(),
            n,
            m_col_scales_.data(),
            m_col_scales_.size(),
            ScaledView::Op::divide,
            m_scale_objective_};
  }

  /**
   * @brief Gets the physical-to-LP scale factor for a single column.
   *
   * physical_value = LP_value × scale.  A scale of 1.0 means no scaling
   * (LP variable == physical value).  Scale factors are populated during
   * load_flat() from FlatLinearProblem::col_scales.
   *
   * @param index Column index
   * @return Scale factor (1.0 if col_scales is empty or not populated)
   */
  [[nodiscard]] constexpr double get_col_scale(ColIndex index) const noexcept
  {
    if (static_cast<size_t>(index) < m_col_scales_.size()) {
      return m_col_scales_[index];
    }
    return 1.0;
  }

  /**
   * @brief Sets the physical-to-LP scale factor for a single column.
   *
   * Use this for columns added dynamically (via add_col) that need a
   * non-unit scale.  Grows the internal scale vector as needed.
   *
   * @param index Column index
   * @param scale Physical-to-LP scale factor (physical = LP × scale)
   */
  void set_col_scale(ColIndex index, double scale) noexcept
  {
    const auto sz = static_cast<size_t>(index) + 1;
    if (sz > m_col_scales_.size()) {
      m_col_scales_.resize(sz, 1.0);
    }
    m_col_scales_[index] = scale;
  }

  /**
   * @brief Gets all column scale factors.
   * @return Const reference to the column scale vector (empty if not
   * populated)
   */
  [[nodiscard]] constexpr const auto& get_col_scales() const noexcept
  {
    return m_col_scales_;
  }

  /**
   * @brief Gets the row equilibration scale factor for a single row.
   *
   * Row equilibration divides each row by s = max|coeff|, so the LP dual
   * is π_LP = s × π_phys.  get_row_scale() returns s.
   *
   * @param index Row index
   * @return Scale factor (1.0 if row_scales is empty or not populated)
   */
  [[nodiscard]] constexpr double get_row_scale(RowIndex index) const noexcept
  {
    if (static_cast<size_t>(index) < m_row_scales_.size()) {
      return m_row_scales_[index];
    }
    return 1.0;
  }

  /**
   * @brief Sets the row scale factor for a single row.
   *
   * Use this for rows added dynamically (via add_row) that need a
   * non-unit scale.  Grows the internal scale vector as needed.
   *
   * @param index Row index
   * @param scale Row scale factor (physical_rhs = LP_rhs × scale)
   */
  void set_row_scale(RowIndex index, double scale) noexcept
  {
    const auto sz = static_cast<size_t>(index) + 1;
    if (sz > m_row_scales_.size()) {
      m_row_scales_.resize(sz, 1.0);
    }
    m_row_scales_[index] = scale;
  }

  /**
   * @brief Gets all row equilibration scale factors.
   * @return Const reference to the row scale vector (empty if not populated)
   */
  [[nodiscard]] constexpr const auto& get_row_scales() const noexcept
  {
    return m_row_scales_;
  }

  /**
   * @brief Gets the global objective scaling factor.
   *
   * obj_physical = obj_LP × scale_objective.
   * Set during load_flat() from FlatLinearProblem::scale_objective.
   * @return The objective scale factor (1.0 if not set)
   */
  [[nodiscard]] constexpr double scale_objective() const noexcept
  {
    return m_scale_objective_;
  }

  /// VariableScaleMap moved from FlatLinearProblem during load_flat().
  [[nodiscard]] const VariableScaleMap& variable_scale_map() const noexcept
  {
    return m_variable_scale_map_;
  }

  /** @brief Lazily compute vertex duals via crossover if the backend
   *  lacks them (barrier without crossover).  No-op when has_duals()
   *  is already true (simplex, or barrier with crossover).
   */
  void ensure_duals();

  /**
   * @brief Gets raw solver dual values (no descaling).
   *
   * Returns solver duals as-is.  Use this for internal LP algebra
   * that operates in solver coordinates (e.g. cut coefficient
   * computation where row equilibration is accounted for separately).
   * @return Span view of raw solver dual values
   */
  [[nodiscard]] auto get_row_dual_raw() -> std::span<const double>
  {
    if (m_backend_released_) {
      return m_cached_row_dual_;
    }
    ensure_duals();
    return {m_backend_->row_price(), get_numrows()};
  }

  /**
   * @brief Gets physical dual values (shadow prices).
   *
   * When row equilibration is active, the raw solver duals are divided
   * by the per-row scale factor to recover physical units.
   * Row equilibration divides each row by s = max|coeff|, so the LP dual
   * is π_LP = s × π_phys, hence π_phys = π_LP / s.
   * The global scale_objective factor is also applied:
   *   dual_physical = dual_LP × scale_objective / row_scale.
   * @return Zero-copy lazy view per element.
   */
  [[nodiscard]] ScaledView get_row_dual()
  {
    if (m_backend_released_) {
      return {m_cached_row_dual_.data(),
              m_cached_row_dual_.size(),
              m_row_scales_.data(),
              m_row_scales_.size(),
              ScaledView::Op::divide,
              m_scale_objective_};
    }
    ensure_duals();
    const auto n = get_numrows();
    return {m_backend_->row_price(),
            n,
            m_row_scales_.data(),
            m_row_scales_.size(),
            ScaledView::Op::divide,
            m_scale_objective_};
  }

  void set_col_sol(std::span<const double> sol);
  void set_row_dual(std::span<const double> dual);

  void set_log_file(std::string_view plog_file);
  [[nodiscard]] constexpr const auto& get_log_file() const
  {
    return m_log_file_;
  }

  void set_prob_name(const std::string& pname);
  [[nodiscard]] std::string get_prob_name() const;

  /// Set a human-readable log tag (e.g. "SDDP Forward [i0 s1 p2]") that
  /// prefixes solver warnings emitted by `initial_solve()` / `resolve()`.
  /// When empty, warnings fall back to `get_prob_name()`.  Callers are
  /// expected to set this before each solve so fallback messages carry
  /// the same context as the surrounding SDDP/monolithic info logs.
  void set_log_tag(std::string_view tag) { m_log_tag_.assign(tag); }

  [[nodiscard]] constexpr const std::string& get_log_tag() const noexcept
  {
    return m_log_tag_;
  }

  /**
   * @brief Sets the LabelMaker used to generate and gate LP col/row labels.
   *
   * The LabelMaker carries the `LpNamesLevel` that controls which labels
   * are produced for `add_col()` / `add_row()` and whether duplicate names
   * are treated as errors.  It is normally installed automatically by
   * `load_flat()` (which copies it from `FlatLinearProblem::label_maker`)
   * but can also be set explicitly via this setter for tests or specialized
   * callers.
   *
   * @param lm The LabelMaker to use (stored by value)
   */
  void set_label_maker(LabelMaker lm) noexcept { m_label_maker_ = lm; }

  /// @brief Returns the LabelMaker driving label generation for add_col/row.
  [[nodiscard]] constexpr const LabelMaker& label_maker() const noexcept
  {
    return m_label_maker_;
  }

  /// @name Name-to-index maps (col: level >= 0, row: level >= 1)
  /// @{

  /// Row (constraint) name → row index map.
  using name_index_map_t = std::unordered_map<std::string, int32_t>;

  [[nodiscard]] constexpr const name_index_map_t& row_name_map() const noexcept
  {
    return m_row_names_;
  }

  [[nodiscard]] constexpr const name_index_map_t& col_name_map() const noexcept
  {
    return m_col_names_;
  }

  /// Column index → name vector (empty string for unnamed columns).
  /// Populated alongside col_name_map when lp_names_level >= 1.
  [[nodiscard]] constexpr const auto& col_index_to_name() const noexcept
  {
    return m_col_index_to_name_;
  }

  /// Row index → name vector (empty string for unnamed rows).
  /// Populated alongside row_name_map when lp_names_level >= 1.
  [[nodiscard]] constexpr const auto& row_index_to_name() const noexcept
  {
    return m_row_index_to_name_;
  }
  /// @}

  /// @name Warm column solution (hot-start state)
  /// @{

  /// Return the warm column solution vector (empty if no state loaded).
  [[nodiscard]] constexpr const auto& warm_col_sol() const noexcept
  {
    return m_warm_col_sol_;
  }

  /// Set the warm column solution from a loaded state file.
  void set_warm_col_sol(StrongIndexVector<ColIndex, double> sol) noexcept
  {
    m_warm_col_sol_ = std::move(sol);
  }
  /// @}

  /// @name LP coefficient statistics (populated during load_flat from
  ///       FlatLinearProblem::stats_* fields, which are computed in
  ///       LinearProblem::flatten when LpMatrixOptions::compute_stats is true).
  /// @{
  [[nodiscard]] constexpr size_t lp_stats_nnz() const noexcept
  {
    return m_stats_nnz_;
  }
  [[nodiscard]] constexpr size_t lp_stats_zeroed() const noexcept
  {
    return m_stats_zeroed_;
  }
  [[nodiscard]] constexpr double lp_stats_max_abs() const noexcept
  {
    return m_stats_max_abs_;
  }
  [[nodiscard]] constexpr double lp_stats_min_abs() const noexcept
  {
    return m_stats_min_abs_;
  }
  [[nodiscard]] constexpr FlatLinearProblem::index_t lp_stats_max_col()
      const noexcept
  {
    return m_stats_max_col_;
  }
  [[nodiscard]] constexpr FlatLinearProblem::index_t lp_stats_min_col()
      const noexcept
  {
    return m_stats_min_col_;
  }
  [[nodiscard]] constexpr const std::string& lp_stats_max_col_name()
      const noexcept
  {
    return m_stats_max_col_name_;
  }
  [[nodiscard]] constexpr const std::string& lp_stats_min_col_name()
      const noexcept
  {
    return m_stats_min_col_name_;
  }
  [[nodiscard]] constexpr const auto& lp_row_type_stats() const noexcept
  {
    return m_row_type_stats_;
  }
  /// @}

private:
  /// @name Legacy column/row helpers (used internally and by tests)
  /// @{
  ColIndex add_col(const std::string& name);
  ColIndex add_col(const std::string& name, double collb, double colub);
  ColIndex add_free_col(const std::string& name);
  RowIndex add_row(const std::string& name,
                   size_t numberElements,
                   const std::span<const int>& columns,
                   const std::span<const double>& elements,
                   double rowlb,
                   double rowub);
  /// @}

  void rebuild_row_name_maps();
  void push_names_to_solver() const;

  struct HandlerGuard
  {
    LinearInterface* interface;

    HandlerGuard(HandlerGuard const&) = delete;
    HandlerGuard& operator=(HandlerGuard const&) = delete;
    HandlerGuard(HandlerGuard&&) = delete;
    HandlerGuard& operator=(HandlerGuard&&) = delete;

    constexpr explicit HandlerGuard(LinearInterface& pinterface, int log_level)
        : interface(&pinterface)
    {
      interface->open_log_handler(log_level);
    }

    ~HandlerGuard() { interface->close_log_handler(); }
  };

  /// RAII guard for solver-native file-based logging (set_log_filename API).
  struct LogFileGuard
  {
    LinearInterface* interface;

    LogFileGuard(LogFileGuard const&) = delete;
    LogFileGuard& operator=(LogFileGuard const&) = delete;
    LogFileGuard(LogFileGuard&&) = delete;
    LogFileGuard& operator=(LogFileGuard&&) = delete;

    explicit LogFileGuard(LinearInterface& li,
                          const std::string& filename,
                          int level)
        : interface(&li)
    {
      interface->m_backend_->set_log_filename(filename, level);
    }

    ~LogFileGuard() { interface->m_backend_->clear_log_filename(); }
  };

  void open_log_handler(int log_level);
  void close_log_handler();

  std::unique_ptr<SolverBackend> m_backend_;
  std::string m_solver_name_ {};  ///< Solver name for backend reconstruction
  std::string m_solver_version_ {};  ///< Cached version for released backends
  SolverOptions m_last_solver_options_ {};  ///< Options from last solve
  std::string m_log_file_ {};
  std::string m_log_tag_ {};  ///< Context tag prefixed to fallback warnings
  LabelMaker m_label_maker_ {};  ///< Label generator + level gate

  /// Name-to-index maps for duplicate detection and later lookup.
  /// Populated when lp_names_level >= 1.
  name_index_map_t m_row_names_;  ///< Row (constraint) name → row index
  name_index_map_t m_col_names_;  ///< Column (variable) name → col index
  StrongIndexVector<ColIndex, std::string> m_col_index_to_name_;
  StrongIndexVector<RowIndex, std::string> m_row_index_to_name_;

  size_t m_base_numrows_ {};  ///< Row count before any cuts were added

  double m_scale_objective_ {1.0};  ///< Global objective divisor (from flatten)
  StrongIndexVector<ColIndex, double> m_col_scales_;
  StrongIndexVector<RowIndex, double> m_row_scales_;
  VariableScaleMap m_variable_scale_map_ {};  ///< Moved from flatten

  /// Warm column solution loaded from a previous run's state file.
  /// Used by StorageLP::physical_eini/efin as fallback when
  /// !is_optimal() (hot start before first solve).  Empty if no
  /// state was loaded.
  StrongIndexVector<ColIndex, double> m_warm_col_sol_;

  size_t m_stats_nnz_ {};
  size_t m_stats_zeroed_ {};
  double m_stats_max_abs_ {};
  double m_stats_min_abs_ {};
  FlatLinearProblem::index_t m_stats_max_col_ {-1};
  FlatLinearProblem::index_t m_stats_min_col_ {-1};
  std::string m_stats_max_col_name_ {};
  std::string m_stats_min_col_name_ {};
  std::vector<FlatLinearProblem::RowTypeStatsEntry> m_row_type_stats_ {};

  struct FILEcloser
  {
    auto operator()(auto f) const { return fclose(f); }
  };
  using log_file_ptr_t = std::unique_ptr<FILE, FILEcloser>;
  log_file_ptr_t m_log_file_ptr_;

  /// Ensure the backend is live; reconstruct from snapshot if released.
  /// Call before any mutation or solve.
  void ensure_backend();

  /// Cache post-solve state and auto-release backend if low_memory is on.
  void cache_and_release();

  // ── Low-memory state ──────────────────────────────────────────────────

  LowMemoryMode m_low_memory_mode_ {LowMemoryMode::off};
  CompressionCodec m_memory_codec_ {CompressionCodec::lz4};
  bool m_cache_warm_start_ {true};

  /// Snapshot: flat LP + cached solution, with compress/decompress support.
  LowMemorySnapshot m_snapshot_ {};

  /// Columns added after initial load_flat() (typically just alpha).
  std::vector<SparseCol> m_dynamic_cols_ {};

  /// Net active Benders cuts (additions minus deletions).
  std::vector<SparseRow> m_active_cuts_ {};

  /// Whether the backend is currently released.
  bool m_backend_released_ {false};

  // ── Cached post-solve state (valid when backend is released) ────────

  /// Cached primal solution from last successful solve.
  std::vector<double> m_cached_col_sol_ {};
  /// Cached dual values from last successful solve.
  std::vector<double> m_cached_row_dual_ {};
  /// Cached reduced costs from last successful solve.
  std::vector<double> m_cached_col_cost_ {};
  /// Cached objective value from last successful solve.
  double m_cached_obj_value_ {};
  /// Cached kappa (condition number) from last successful solve.
  double m_cached_kappa_ {-1.0};
  /// Cached number of rows at time of release.
  size_t m_cached_numrows_ {};
  /// Cached number of columns at time of release.
  size_t m_cached_numcols_ {};
  /// Whether the cached state represents an optimal solution.
  bool m_cached_is_optimal_ {false};
};

/// RAII guard that decompresses a LinearInterface's flat LP on construction
/// and re-compresses it on destruction.  Use this to keep the flat LP
/// uncompressed while creating multiple clones from the live backend.
///
/// Example:
/// @code
///   {
///     DecompressionGuard guard(li);
///     // All clones created here avoid per-clone decompress overhead
///     auto c1 = li.clone(col_sol, row_dual);
///     auto c2 = li.clone(col_sol, row_dual);
///   }  // flat LP re-compressed here
/// @endcode
class DecompressionGuard
{
public:
  explicit DecompressionGuard(LinearInterface& li) noexcept
      : m_li_(li)
  {
    m_li_.disable_compression();
  }

  ~DecompressionGuard() noexcept { m_li_.enable_compression(); }

  DecompressionGuard(const DecompressionGuard&) = delete;
  DecompressionGuard& operator=(const DecompressionGuard&) = delete;
  DecompressionGuard(DecompressionGuard&&) = delete;
  DecompressionGuard& operator=(DecompressionGuard&&) = delete;

private:
  LinearInterface& m_li_;
};

}  // namespace gtopt
