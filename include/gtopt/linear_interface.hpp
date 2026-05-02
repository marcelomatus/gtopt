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

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtopt/error.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/lp_validation.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_backend.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_stats.hpp>
#include <gtopt/strong_index_vector.hpp>

namespace gtopt
{

/// Zero-copy lazy view that applies a per-element scale factor.
///
/// Models a random-access range: `view[i]` returns `data[i] * scales[i]`
/// (or `data[i] / scales[i]` when constructed with `divides`).
/// When scales are empty, returns raw data unchanged.
///
/// Optional physical-space clamp: when `lower_` and `upper_` spans are
/// both non-empty, `operator[](i)` additionally clamps the result to
/// `[lower_[i] * scales_[i], upper_[i] * scales_[i]]` — i.e., the clamp
/// is applied *after* descaling, in the same physical space the caller
/// sees.  Used by `LinearInterface::get_col_sol()` to scrub solver
/// noise (value returned slightly outside its bound-box on an optimal
/// solve) so downstream SDDP state propagation can pin clean values
/// without causing next-phase infeasibility.
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

  // `n` / `nlo` / `nup` use `Index` (signed int32) to match the LP layer
  // (`LinearInterface::get_numrows()` / `get_numcols()` and the solver
  // backends).  `ns` stays `size_t` to match `std::vector::size()` which
  // is the natural source of the scales-vector length.  The narrowing
  // happens implicitly inside the `std::span(ptr, n)` paren-init below
  // — single point of conversion, no caller-side casts needed.
  constexpr ScaledView(const double* data,
                       Index n,
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

  /// Construct with physical-space clamp bounds (lower/upper in the
  /// same raw-LP space as `data`; each element is descaled by `scales`
  /// before clamping).  When either span is empty, clamping is skipped.
  constexpr ScaledView(const double* data,
                       Index n,
                       const double* scales,
                       size_t ns,
                       const double* lower,
                       Index nlo,
                       const double* upper,
                       Index nup,
                       Op op = Op::multiply,
                       double global_factor = 1.0) noexcept
      : data_(data, n)
      , scales_(scales, ns)
      , lower_(lower, nlo)
      , upper_(upper, nup)
      , op_(op)
      , global_(global_factor)
  {
  }

  /// Construct from a raw span (no scaling).
  constexpr explicit ScaledView(std::span<const double> raw) noexcept
      : data_(raw)
  {
  }

  /// Accepts any integer-like type (int, size_t, ColIndex, RowIndex, …).
  template<typename T>
    requires std::is_convertible_v<T, size_t>
  [[nodiscard]] constexpr double operator[](T idx) const noexcept
  {
    const auto i = static_cast<size_t>(idx);
    const double scale = (i < scales_.size()) ? scales_[i] : 1.0;
    const double v = (i < scales_.size() && op_ == Op::divide)
        ? data_[i] / scale
        : data_[i] * scale;
    double result = v * global_;
    // Physical-space clamp: applied AFTER descaling so no further
    // multiplication can re-violate the bound box.
    if (i < lower_.size() && i < upper_.size()) {
      const double lb_phys = lower_[i] * scale;
      const double ub_phys = upper_[i] * scale;
      if (lb_phys <= ub_phys) {  // guard against degenerate/inverted bounds
        result = std::clamp(result, lb_phys, ub_phys);
      }
    }
    return result;
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
  std::span<const double>
      lower_ {};  ///< Optional raw-LP lower bounds for clamp
  std::span<const double>
      upper_ {};  ///< Optional raw-LP upper bounds for clamp
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

class SystemLP;  // owner for the rebuild-mode back-pointer

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

  /// Selects how `clone()` replicates the metadata (label vectors,
  /// dedup maps, scale vectors).  The backend solver handle is always
  /// cloned independently — solver internals (CPLEX env, HiGHS basis
  /// factorisation) are non-reentrant, so each clone needs its own
  /// independent backend regardless.
  enum class CloneKind : std::uint8_t
  {
    /// Each metadata structure is value-copied into the clone — the
    /// resulting clone owns an independent copy of every field.  Use
    /// for bad-LP dumps where the dump must remain stable even if the
    /// source mutates afterward.  Current default.
    deep,

    /// Metadata structures are shared with the source via
    /// `std::shared_ptr`.  Cheap to construct (atomic incref per
    /// field).  Mutating either side detaches its own shared_ptr
    /// before mutating, so the other side stays unaffected — but the
    /// hot paths (aperture solve, elastic filter slack inserts via
    /// `add_*_disposable`) NEVER mutate the shared metadata, so the
    /// detach branch is dormant in practice.
    ///
    /// Precondition: the source must not be concurrently mutated for
    /// the lifetime of the shallow clone.  In gtopt's SDDP this is
    /// enforced by `DecompressionGuard` around the aperture pass —
    /// see `sddp_aperture_pass.cpp:390, 579`.
    shallow,
  };

  /**
   * @brief Creates a copy of this LinearInterface via the backend's
   *        clone() method.
   *
   * The clone preserves the full LP state (variables, constraints, bounds,
   * objective, warm-start basis).  Modifications to the clone's bounds /
   * objective do not affect the original — use this for tentative solves
   * such as the SDDP elastic filter or aperture sub-problems.
   *
   * `kind == CloneKind::deep` (default) replicates the historical
   * behaviour: every metadata vector / map is value-copied.
   * `kind == CloneKind::shallow` shares the metadata via shared_ptr;
   * use for solve-and-discard clones (aperture, elastic) to save
   * memory at peak concurrency.  See `CloneKind` for full contract.
   *
   * @return A new LinearInterface wrapping the cloned solver.
   */
  [[nodiscard]] LinearInterface clone(CloneKind kind = CloneKind::deep) const;

  /**
   * @brief Manual clone — replays this LI's saved `FlatLinearProblem`
   *        through `load_flat()` on a fresh `LinearInterface`, bypassing
   *        the backend's native `clone()` entirely.
   *
   * Why two clone routes coexist:
   *   - `clone(CloneKind)`        → calls `backend().clone()` (e.g.
   *     `CPXcloneprob`).  Some backends (CPLEX 22.1 in particular)
   *     have process-global side effects in this path that crashed
   *     three threads at the same instruction pointer in juan/
   *     gtopt_iplp (commit 1d7a05c1) — the call site in
   *     `sddp_aperture.cpp` therefore serialises every `clone()`
   *     invocation under `s_global_clone_mutex`.
   *   - `clone_from_flat(kind)` → goes through `CPXcreateprob` +
   *     `CPXaddrows` (or the equivalent on each backend).  Those
   *     calls are env-local: every `CplexSolverBackend` ctor runs
   *     them concurrently during plugin load with no mutex, so the
   *     manual route is structurally safe to call without any lock.
   *
   * Pre-condition:
   *   - `has_snapshot_data()` must be true — i.e. the LI has been
   *     frozen with `LowMemoryMode::compress` (or `snapshot`) so
   *     the flat representation is retained.  Throws
   *     `std::runtime_error` otherwise.
   *   - `m_snapshot_` must be decompressed.  In SDDP this is
   *     enforced for the aperture window via the existing
   *     `DecompressionGuard` at `sddp_aperture_pass.cpp:390, 579`.
   *     Throws `std::runtime_error` if compressed.
   *
   * `kind == CloneKind::shallow` re-shares the source's metadata
   * shared_ptrs into the freshly built clone after `load_flat`,
   * matching the memory-saving semantics of the native shallow
   * path.  `kind == CloneKind::deep` keeps the freshly-built
   * metadata that `load_flat` produced.
   *
   * @return A new LinearInterface populated from the saved snapshot.
   */
  [[nodiscard]] LinearInterface clone_from_flat(
      CloneKind kind = CloneKind::shallow) const;

  /// Test/diagnostic accessor: `shared_ptr::use_count()` of one of
  /// the wrapped metadata members.  All of them are detached together
  /// in `clone(deep)` and shared together in `clone(shallow)`, so any
  /// one is representative.  Use cases:
  ///
  ///   - Unit tests verifying the sharing contract (after
  ///     `clone(shallow)` it should be 2; after `clone(deep)` 1;
  ///     after a mutation through `detach_for_write` it drops back
  ///     to 1 on whichever member was touched).
  ///   - Diagnostics logging memory-saving hit rate at peak aperture
  ///     concurrency.
  ///
  /// Reads `m_col_labels_meta_` because that's the largest-footprint
  /// member and the most useful to track, but any of the eleven
  /// wrapped members would do.
  [[nodiscard]] auto col_labels_meta_use_count() const noexcept
  {
    return m_col_labels_meta_.use_count();
  }
  [[nodiscard]] auto col_scales_use_count() const noexcept
  {
    return m_col_scales_.use_count();
  }
  [[nodiscard]] auto row_scales_use_count() const noexcept
  {
    return m_row_scales_.use_count();
  }
  [[nodiscard]] auto col_meta_index_use_count() const noexcept
  {
    return m_col_meta_index_.use_count();
  }
  [[nodiscard]] auto row_meta_index_use_count() const noexcept
  {
    return m_row_meta_index_.use_count();
  }

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

  /// Drop the cached primal vectors (`col_sol`, `col_cost`) but keep
  /// `row_dual` (and the scalar caches).
  ///
  /// `release_backend()` populates three primal/dual snapshots so that
  /// post-release readers can keep accessing the solution:
  ///   - `col_sol` and `col_cost` are read by `OutputContext`,
  ///     `save_state_csv`, and the SDDP state-variable propagation
  ///     paths during the iteration that produced the solve.
  ///   - `row_dual` is read **across iterations** by
  ///     `update_stored_cut_duals` and `prune_inactive_cuts`
  ///     (`sddp_cut_store.cpp:177, 222`).
  ///
  /// On a juan/gtopt_iplp run the col_sol + col_cost vectors carry
  /// 0.7–2.5 GB across all `(scene, phase)` cells between iterations.
  /// Once the iteration's state CSV has been written and the backward
  /// pass has consumed the per-state-variable values via
  /// `capture_state_variable_values`, neither vector is read again
  /// until the next solve overwrites it — making them the largest
  /// "alive but unused" allocation in the SDDP loop.
  ///
  /// Call this after `save_cuts_for_iteration` (state CSV) returns
  /// and before the next iteration's forward pass begins.  Safe under
  /// `low_memory_mode == off` (no-op since the caches stay empty)
  /// and idempotent.  Does NOT touch `m_cached_row_dual_`,
  /// `m_cached_obj_value_`, `m_cached_kappa_`, `m_cached_numrows_`,
  /// `m_cached_numcols_`, or `m_cached_is_optimal_` — those remain
  /// available for inter-iteration consumers.
  void drop_cached_primal_only() noexcept
  {
    m_cached_col_sol_.clear();
    m_cached_col_sol_.shrink_to_fit();
    m_cached_col_cost_.clear();
    m_cached_col_cost_.shrink_to_fit();
  }

  /// Drop the label-metadata buffers (compressed col/row label vectors
  /// and the string pool that backs the decompressed `string_view`s).
  ///
  /// `compress_labels_meta_if_needed()` populates these buffers on
  /// every `release_backend()` so future readers — `write_lp` (debug
  /// LP dump), cut JSON / CSV save, `OutputContext::write_out` —
  /// can resolve LP row / column names without re-flattening from
  /// `System` collections.  Once a run has completed all of those
  /// operations, the buffers are dead weight: ~3 MB raw / ~1 MB
  /// compressed per cell, so on a juan/gtopt_iplp scale (816 cells)
  /// they total ~800 MB held until process exit.
  ///
  /// Call this after the final `PlanningLP::write_out` returns and
  /// no further label-metadata reads will occur.  Idempotent and
  /// safe to call on cells that never had label metadata (no-op).
  /// **Do not call mid-run** — `generate_labels_from_maps` (used by
  /// every cut JSON save and every `write_lp`) would then throw
  /// "row N has metadata without a class_name" on the next call.
  void drop_label_meta_buffers() noexcept
  {
    m_col_labels_meta_compressed_ = CompressedBuffer {};
    m_row_labels_meta_compressed_ = CompressedBuffer {};
    m_col_labels_meta_count_ = 0;
    m_row_labels_meta_count_ = 0;
    m_label_string_pool_.clear();
    m_label_string_pool_.shrink_to_fit();
  }

  /**
   * @brief Check whether the solver backend is loaded.
   * @return true if load_flat() has been called and release_backend() has not
   */
  [[nodiscard]] bool has_backend() const noexcept
  {
    return m_backend_ != nullptr;
  }

  // ── Low-memory mode API ─────────────────────────────────────────────────

  /// Lifecycle phase of the LinearInterface.  Advance on every
  /// state-changing entry point so observers (debug logging, future
  /// assertion guards in plan step 4) can see what stage the LP is
  /// in.
  ///
  /// State transitions tracked:
  ///   (default)        → Building          ctor
  ///   Building         → Building          load_flat (still pre-freeze)
  ///   Building         → Frozen            freeze_for_cuts
  ///   Frozen           → BackendReleased   release_backend
  ///   Reconstructed    → BackendReleased   release_backend
  ///   BackendReleased  → Reconstructed     reconstruct_backend
  ///
  /// No transition assertions in this step — step 2 of the
  /// `support/linear_interface_lifecycle_plan_2026-04-30.md` is pure
  /// observation; step 4 layers debug-asserted transitions on top
  /// once every call site has migrated to the canonical entry
  /// points.
  enum class LiPhase : uint8_t
  {
    Building,
    Frozen,
    BackendReleased,
    Reconstructed,
  };

  /// Current lifecycle phase.  Mutates only through the public
  /// methods listed in the `LiPhase` doc; never set externally.
  [[nodiscard]] LiPhase phase() const noexcept { return m_phase_; }

  /// Configure low-memory mode (off, snapshot, compress, rebuild).
  void set_low_memory(LowMemoryMode mode,
                      CompressionCodec codec = CompressionCodec::lz4) noexcept;

  /// Save a flat LP snapshot for future reconstruction.
  /// Call from create_lp() — the LinearInterface decides whether to keep
  /// based on the configured level.
  void save_snapshot(FlatLinearProblem flat_lp);

  /// Single-call entry point for the structural-build → cut-build
  /// transition under low-memory modes.  Replaces the legacy 3-call
  /// dance:
  ///
  ///   set_low_memory(mode, codec);
  ///   save_snapshot(std::move(flat_lp));
  ///   save_base_numrows();
  ///
  /// Call this once after `load_flat` has populated the backend with
  /// the structural rows and before any Benders cut is added.  See
  /// `support/linear_interface_lifecycle_plan_2026-04-30.md` step 1
  /// for the rationale: the legacy three-method ordering is implicit
  /// and easy to get wrong (forgetting `save_base_numrows` mis-attributes
  /// structural rows as cuts; forgetting `save_snapshot` makes
  /// `reconstruct_backend()` a silent no-op).  This consolidator
  /// collapses the dance into one verb.
  ///
  /// Pre-conditions (debug-asserted):
  /// - the backend has been loaded (`m_backend_released_ == false` OR
  ///   `defer_initial_load` has populated `m_cached_numrows_`),
  /// - no cut rows have been added yet (`m_active_cuts_.empty()`).
  void freeze_for_cuts(LowMemoryMode mode,
                       FlatLinearProblem flat_lp,
                       CompressionCodec codec = CompressionCodec::lz4);

  /// Drop the compressed/uncompressed flat-LP snapshot held by this
  /// interface.  Used by the SDDP simulation Pass 1 in low-memory mode
  /// to free the per-cell LP matrix right after the solve caches its
  /// primal/dual/cost vectors via `release_backend()` — subsequent
  /// `PlanningLP::write_out` reads solution values straight from the
  /// cached vectors (see Phase 2a getters) and `rebuild_collections_if_
  /// needed` re-flattens from the live `System` element arrays, so the
  /// flat-LP snapshot is no longer needed for any downstream step.
  ///
  /// Calling this after `release_backend()` is safe; the cached scalars
  /// and vectors stay intact.  After this, a subsequent
  /// `reconstruct_backend()` would have nothing to reload from — only
  /// call when the LP is truly done.
  void clear_snapshot() noexcept { m_snapshot_ = LowMemorySnapshot {}; }

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

  /// Number of structural columns / rows captured at `flatten` time
  /// and still pinned in the frozen `m_col_labels_meta_` /
  /// `m_row_labels_meta_` vector.  Indices in `[0, flatten_col_count())`
  /// are flatten-side; higher indices are post-flatten additions
  /// (alpha, cuts, cascade elastic slacks, …).  `m_col_labels_meta_`
  /// may be nullptr only on a default-constructed `LinearInterface`
  /// before `load_flat`; in that case the count is 0.
  [[nodiscard]] auto flatten_col_count() const noexcept
  {
    return m_col_labels_meta_ ? m_col_labels_meta_->size() : size_t {0};
  }
  [[nodiscard]] auto flatten_row_count() const noexcept
  {
    return m_row_labels_meta_ ? m_row_labels_meta_->size() : size_t {0};
  }

  /// Resolve a column / row index into the appropriate metadata bucket
  /// (frozen flatten or per-instance post-flatten).  Returns `nullptr`
  /// when the index is past both buckets — which is only possible if
  /// the caller is asking about a backend column that was never tracked
  /// (e.g. `add_col(name, lb, ub)` paths that bypass label-meta capture).
  ///
  /// Cheap: a bounds check + a vector index per call, no decompression
  /// on the post-flatten side.  The flatten side may be in compressed
  /// form during training/SDDP; consult `ensure_labels_meta_decompressed`
  /// before calling if formatted strings are needed.
  [[nodiscard]] const SparseColLabel* col_label_at(ColIndex idx) const noexcept;
  [[nodiscard]] const SparseRowLabel* row_label_at(RowIndex idx) const noexcept;

  /// Record a dynamically-added column (e.g. alpha variable).
  void record_dynamic_col(SparseCol col);

  /// Update the `lowb` on the first `m_dynamic_cols_` entry matching
  /// `(class_name, variable_name)` so that a subsequent snapshot reload
  /// (`apply_post_load_replay`) restores the column with the new lower
  /// bound rather than the original one.  Returns `true` iff a matching
  /// entry was found and updated.  No-op under `LowMemoryMode::off`
  /// (where `m_dynamic_cols_` stays empty) — the live backend is the
  /// sole authority and needs no dynamic-col mirror.
  bool update_dynamic_col_lowb(std::string_view class_name,
                               std::string_view variable_name,
                               double new_lowb) noexcept;

  /// Two-sided bound update for a dynamic column — used by the SDDP
  /// `free_alpha` path which relaxes both the pinned `lowb = 0` and
  /// `uppb = 0` bootstrap in a single call.  Semantics mirror
  /// `update_dynamic_col_lowb`: no-op under `LowMemoryMode::off`,
  /// returns `true` iff a matching `(class_name, variable_name)`
  /// entry was found in `m_dynamic_cols_` and both bounds overwritten.
  bool update_dynamic_col_bounds(std::string_view class_name,
                                 std::string_view variable_name,
                                 double new_lowb,
                                 double new_uppb) noexcept;

  /// Record a structural row added after the initial flatten/load_flat
  /// (typically cascade elastic-target constraints).  No-op under
  /// `LowMemoryMode::off`.  The recorded row is replayed in
  /// `apply_post_load_replay` BEFORE `save_base_numrows()` so the
  /// structural-vs-cut boundary correctly classifies it as structural —
  /// this is what distinguishes it from `record_cut_row`, which records
  /// post-baseline rows for the SDDP cut store.
  void record_dynamic_row(SparseRow row);

  /// Record a Benders cut row addition.
  void record_cut_row(SparseRow row);

  /// Pre-reserve capacity for active cuts.  Cut loaders that know the
  /// expected row count ahead of time can call this before their
  /// `add_row` + `record_cut_row` loop to avoid log-N reallocations.
  void reserve_active_cuts(std::size_t n) { m_active_cuts_.reserve(n); }

  /// Record cut row deletions (pruning/forgetting).
  void record_cut_deletion(std::span<const int> deleted_indices);

  /// True when a `FlatLinearProblem` snapshot is currently held.
  /// Always false in `LowMemoryMode::rebuild` after release_backend.
  [[nodiscard]] bool has_snapshot_data() const noexcept
  {
    return m_snapshot_.has_data();
  }

  /// Move out the recorded dynamic columns (alpha) so the caller can
  /// preserve them across a destructive rebuild.  Used by
  /// `SystemLP::ensure_lp_built()` under `LowMemoryMode::rebuild`.
  [[nodiscard]] std::vector<SparseCol> take_dynamic_cols() noexcept
  {
    return std::exchange(m_dynamic_cols_, {});
  }

  /// Move out the active Benders cuts so the caller can preserve them
  /// across a destructive rebuild.
  [[nodiscard]] std::vector<SparseRow> take_active_cuts() noexcept
  {
    return std::exchange(m_active_cuts_, {});
  }

  /// Restore previously-taken dynamic columns (rollback path on rebuild
  /// failure).  Replaces any current m_dynamic_cols_.
  void restore_dynamic_cols(std::vector<SparseCol> cols) noexcept
  {
    m_dynamic_cols_ = std::move(cols);
  }

  /// Restore previously-taken active cuts (rollback path on rebuild
  /// failure).  Replaces any current m_active_cuts_.
  void restore_active_cuts(std::vector<SparseRow> cuts) noexcept
  {
    m_active_cuts_ = std::move(cuts);
  }

  /// Set the base row count explicitly (used after a rebuild restores
  /// state without re-querying the live backend).
  void set_base_numrows(Index n) noexcept
  {
    m_base_numrows_ = n;
    m_base_numrows_set_ = true;
  }

  /// Mark this interface as "no LP loaded": flips `m_backend_released_` to
  /// true and, for non-rebuild modes, drops the default-constructed
  /// backend handle.  Intended for `LowMemoryMode::rebuild`, where
  /// `SystemLP`'s constructor wants to install the low-memory configuration
  /// without paying for an initial `load_flat()` — the next backend access
  /// (via `ensure_backend()`) will invoke the rebuild callback to assemble
  /// the flat LP lazily.  In rebuild mode the default backend handle is
  /// retained so that `infinity()` stays queryable for the LinearProblem
  /// builder used inside the rebuild callback.  Unlike `release_backend()`,
  /// this skips solution caching and snapshot compression: there is nothing
  /// meaningful to cache yet.
  void mark_released() noexcept
  {
    if (m_low_memory_mode_ != LowMemoryMode::rebuild) {
      m_backend_.reset();
    }
    m_backend_released_ = true;
  }

  /// Install the owning SystemLP as the rebuild-source for
  /// `LowMemoryMode::rebuild`.  When the backend is released, the next
  /// `ensure_backend()` call invokes `SystemLP::rebuild_in_place()` on
  /// this pointer.  Must be re-set after every SystemLP move so it
  /// tracks the new object's address.
  ///
  /// Pass `nullptr` to clear — typical under other low_memory modes.
  void set_rebuild_owner(SystemLP* owner) noexcept { m_rebuild_owner_ = owner; }

  /// True iff a rebuild owner has been installed.  Used by SystemLP to
  /// decide whether an accidental pre-owner access would be a logic bug.
  [[nodiscard]] bool has_rebuild_callback() const noexcept
  {
    return m_rebuild_owner_ != nullptr;
  }

  /// Replay the persistent SDDP state (dynamic cols + active cuts +
  /// warm-start) onto the live backend after a fresh `load_flat()`.  Used
  /// by both the compress/snapshot reconstruction path and the rebuild
  /// callback, which share the same post-load protocol: structural rows
  /// are in the backend, and the caller-tracked state must be replayed on
  /// top so subsequent add_col/add_row calls append cleanly.
  ///
  /// Pre-condition: the backend is live (post `load_flat`) and
  /// `m_backend_released_` has been cleared.
  void apply_post_load_replay(std::span<const double> col_sol = {},
                              std::span<const double> row_dual = {});

  /// Finalize a rebuild in place: clear the released flag, run
  /// `load_flat(flat_lp)` on the existing backend, and replay persistent
  /// state (dynamic cols + base_numrows + active cuts).  Used exclusively
  /// by the `LowMemoryMode::rebuild` callback — mirrors
  /// `reconstruct_backend` but takes a caller-provided flat LP instead of
  /// sourcing one from `m_snapshot_`.
  void install_flat_as_rebuild(const FlatLinearProblem& flat_lp);

  /// Ensure the backend is live.  For `LowMemoryMode::off` this is a
  /// no-op (backend is always live).  For `snapshot` / `compress` it
  /// reconstructs from the saved snapshot.  For `rebuild` it invokes
  /// the installed rebuild callback (which flattens the LP from source
  /// collections and installs it via `install_flat_as_rebuild`).
  ///
  /// Any mutation method (add_col, add_row, set_*, solve) calls this
  /// internally, so most callers don't need to invoke it directly.
  /// Useful as an explicit trigger for pure-read code paths that would
  /// otherwise read stale cached row/col counts from a released cell.
  void ensure_backend();

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

  /**
   * @brief Adds a new column WITHOUT extending `m_col_scales_`.
   *
   * Same as `add_col(SparseCol)` but **never** calls `set_col_scale`.
   * Asserts `col.scale == 1.0` so the caller can't accidentally lose
   * a non-unit scale.  Use this for post-flatten col adds that don't
   * need per-col scale tracking — chiefly the α-column registration
   * in `apply_post_load_replay`.
   *
   * Mirrors `add_row_raw`'s contract: emit + label-meta tracking +
   * dedup, but no per-column / per-row scale-vector mutation.  This
   * is the precondition for sharing `m_col_scales_` across aperture
   * clones via `std::shared_ptr` — the scale vector is frozen once
   * `load_flat` has populated it.
   *
   * @param col Sparse column with `scale == 1.0`.
   * @return The index of the newly added column.
   */
  ColIndex add_col_raw(const SparseCol& col);

  /**
   * @brief Bulk-add columns (much faster than repeated add_col calls).
   *
   * Builds a CSC (Compressed Sparse Column) buffer from the provided
   * `SparseCol` objects, applies bound normalisation and the
   * `scale_objective` divisor on each objective coefficient, dispatches
   * a single `SolverBackend::add_cols(...)` call, and then performs the
   * per-column post-bookkeeping (column-scale registration, label-meta
   * tracking with dedup, and optional name generation) — mirroring how
   * `add_rows(span<SparseRow>)` shapes the row-bulk path.
   *
   * `SparseCol` carries no coefficient map, so each column's CSC slice
   * is empty (`colbeg[c+1] == colbeg[c]`).  The bulk path's value is
   * the single backend dispatch: each per-column call typically
   * reallocates the backend's internal column metadata array.
   *
   * Mirrors `add_col(SparseCol)`'s contract: every column in the batch
   * routes through the same `cost / scale_objective` composition, and
   * any non-unit `col.scale` extends `m_col_scales_` via
   * `set_col_scale` (just like the singular path).  Use this for
   * batches of physical-space columns (the typical case).  For LP-raw
   * batches that must skip `m_col_scales_` extension, use
   * `add_cols_raw` instead.
   *
   * @param cols Sparse columns with physical-space cost / bounds.
   */
  void add_cols(std::span<const SparseCol> cols);

  /**
   * @brief Bulk-add columns WITHOUT extending `m_col_scales_`.
   *
   * Companion to `add_col_raw` for batches.  Same `cost /
   * scale_objective` composition as `add_cols` (i.e. `col.cost` is
   * still treated as physical and divided by `m_scale_objective_`),
   * but `m_col_scales_` is never grown — so every entry must satisfy
   * `col.scale == 1.0`.  Used by post-flatten cut paths that need the
   * column scale vector to stay frozen so it can be shared across
   * aperture clones via `std::shared_ptr`.
   *
   * @param cols Sparse column specifications with `scale == 1.0`.
   */
  void add_cols_raw(std::span<const SparseCol> cols);

  /**
   * @brief Adds a new constraint row to the problem
   * @param row The sparse row representation of the constraint
   * @param eps Epsilon value for coefficient filtering (values below this are
   * ignored)
   * @return The index of the newly added row
   */
  /**
   * @brief Adds a new constraint row to the problem.
   *
   * Coefficients are interpreted as **physical-space** by default.
   * When the LP was built with equilibration (row_max or ruiz) and
   * `save_base_numrows()` has fired (i.e. this row is a post-build
   * cut), `add_row` applies, in order:
   *
   *  1. Physical → LP column scaling: `coeff_j ← coeff_j ×
   *     col_scales[j]`.  Bounds are not touched here.
   *  2. Per-row row-max normalisation: divide coefficients and bounds
   *     by `max|coeff_j|`, so the new row lands at `max|coeff| = 1`,
   *     matching the structural rows processed by
   *     `apply_row_max_equilibration` / `apply_ruiz_scaling` at build
   *     time.
   *  3. Composite row scale storage: `row_scale = row.scale ×
   *     max|coeff|`, preserving `dual_phys = dual_LP × row_scale`.
   *
   * Otherwise (no equilibration on the LP, or this row still belongs
   * to the structural-build phase), `add_row` is a pass-through — the
   * physical space and LP space coincide, so no conversion happens.
   *
   * @param row The constraint with physical-space coefficients.
   * @param eps Epsilon value for coefficient filtering (values below
   *            are ignored).
   * @return The index of the newly added row.
   */
  RowIndex add_row(const SparseRow& row, double eps = 0.0);

  /**
   * @brief Add a row in LP-raw space (skip col_scale / row-max
   *        equilibration / scale_objective composition).
   *
   * Companion to ``get_col_low_raw`` / ``get_col_sol_raw`` /
   * ``get_row_dual_raw``: callers that have already computed
   * coefficients and bounds in the LP solver's own space (after
   * ``flatten()`` has run) and want them inserted verbatim should
   * use this instead of ``add_row``.  Only ``SparseRow::scale`` is
   * composed (mirroring ``flatten()``'s treatment of static rows);
   * no per-column ``col_scale`` multiplication, no per-row
   * equilibration, no ``scale_objective`` divisor.
   *
   * Use case: SDDP elastic-filter fixing rows.  ``add_row`` in the
   * post-flatten ("cut phase") path applies all three transforms,
   * which silently breaks the fixing-row semantics ``dep + sup −
   * sdn = trial`` whenever ``col_scale(dep) != 1`` — the slacks end
   * up scaled out of step with the dependent column.  ``add_row_raw``
   * preserves the unit coefficients and the LP-raw RHS.
   *
   * @param row Constraint with LP-raw coefficients and LP-raw bounds.
   * @param eps Epsilon for coefficient filtering.
   */
  RowIndex add_row_raw(const SparseRow& row, double eps = 0.0);

  /**
   * @brief Add a disposable column on a shallow clone.
   *
   * Goes DIRECTLY to the solver backend (`m_backend_->add_col`) —
   * does not transit through `add_col`, `add_col_raw`, or
   * `emit_col_to_backend`.  Captures only the label-meta fields of
   * `col` (class_name, variable_name, variable_uid, context) into a
   * per-clone-local extras vector + dedup map.  Never touches the
   * shared `m_col_labels_meta_`, `m_col_meta_index_`, or
   * `m_col_scales_` — designed for use on a `clone(CloneKind::shallow)`
   * where those structures are shared read-only with the source.
   *
   * Asserts `col.scale == 1.0` (the elastic-filter slack convention)
   * — non-unit scales would require extending `m_col_scales_`, which
   * the disposable contract forbids.
   *
   * `generate_labels_from_maps` consults the per-clone extras when
   * the col index is past the end of the shared metadata, so
   * `write_lp` produces gtopt-formatted labels for disposable cols
   * identical to what the production path would emit.
   *
   * Use case: SDDP elastic-filter slack cols on the cloned LP.
   * Solve-and-discard pattern, ≤ 20 cols per call.
   *
   * @param col SparseCol with `scale == 1.0`.
   * @return Index of the newly added column.
   */
  ColIndex add_col_disposable(const SparseCol& col);

  /**
   * @brief Bulk-add disposable columns on a shallow clone.
   *
   * Mirrors `add_col_disposable` for batches: each column goes
   * directly to the solver backend with `cost / scale_objective`
   * composition; label-meta is captured into the per-clone-local
   * extras (`m_post_clone_col_metas_*`).  Asserts every entry has
   * `scale == 1.0`.  Returns the freshly assigned indices in the
   * same order as `cols`.
   *
   * @param cols SparseCols with `scale == 1.0`.
   * @return Vector of newly assigned column indices.
   */
  std::vector<ColIndex> add_cols_disposable(std::span<const SparseCol> cols);

  /**
   * @brief Add a disposable row on a shallow clone.
   *
   * Companion to `add_col_disposable`.  Goes DIRECTLY to
   * `m_backend_->add_row`; captures only the label-meta fields of
   * `row` into a per-clone-local extras vector + dedup map.  Never
   * touches the shared `m_row_labels_meta_`, `m_row_meta_index_`,
   * or `m_row_scales_`.
   *
   * Asserts `row.scale == 1.0` (the elastic-filter fixing-row
   * convention).
   *
   * Use case: SDDP elastic-filter fixing rows on the cloned LP.
   *
   * @param row SparseRow with `scale == 1.0`.
   * @param eps Epsilon for coefficient filtering.
   * @return Index of the newly added row.
   */
  RowIndex add_row_disposable(const SparseRow& row, double eps = 0.0);

  /**
   * @brief Bulk-add disposable rows on a shallow clone.
   *
   * Mirrors `add_row_disposable` for batches: every row goes
   * directly to the solver backend (LP-raw insertion, only
   * `SparseRow::scale` composed); label-meta is captured into the
   * per-clone-local extras (`m_post_clone_row_metas_*`).  Asserts
   * every entry has `scale == 1.0`.  Returns the freshly assigned
   * indices in the same order as `rows`.
   *
   * @param rows SparseRows with `scale == 1.0` (LP-raw coefficients).
   * @param eps  Epsilon for coefficient filtering.
   * @return Vector of newly assigned row indices.
   */
  std::vector<RowIndex> add_rows_disposable(std::span<const SparseRow> rows,
                                            double eps = 0.0);

  /**
   * @brief Add a cut row AND record it for low-memory replay.
   *
   * Equivalent to `add_row(row, eps)` followed by `record_cut_row(row)`
   * — every cut install site needs both so the row survives a
   * `release_backend` / `ensure_backend` cycle by being replayed
   * through `apply_post_load_replay`.  Prefer this over the bare
   * `add_row` + `record_cut_row` pair for new code.  Generated-cut
   * paths that also push into `SDDPMethod::m_cut_store_` continue to
   * use `add_row` + `store_cut` (which wraps `record_cut_row`
   * internally) to avoid double-recording.
   *
   * @param row Cut row, physical-space coefficients.
   * @param eps Coefficient filtering threshold.  Callers should pass
   *            the active `SDDPOptions::cut_coeff_eps` so loaded and
   *            generated cuts see the same filtering.  Defaults to 0
   *            (no filtering) for compatibility with existing
   *            loaders whose signatures don't plumb `cut_coeff_eps`
   *            yet — follow-up refactor should thread that option
   *            through each loader's parameter list.
   * @return Row index assigned by the solver backend.
   */
  RowIndex add_cut_row(const SparseRow& row, double eps = 0.0)
  {
    const auto idx = add_row(row, eps);
    // `record_cut_row` no-ops when `m_low_memory_mode_ == off`.
    record_cut_row(row);
    return idx;
  }

  /**
   * @brief Bulk-add constraint rows (much faster than repeated add_row calls).
   *
   * Mirrors `add_row`'s contract for batches: when the LP is in the
   * post-flatten cut phase (`save_base_numrows()` has fired) AND
   * `m_col_scales_` / equilibration are active, every row in the batch
   * is treated as **physical-space** and routed through the per-row
   * compose_physical transform (col_scale × elem / scale_objective /
   * row-max).  Otherwise the bulk CSR fast path runs unchanged.
   *
   * Use this for batches of physical-space rows (cuts, post-flatten
   * user-built rows).  For LP-raw batches that should bypass
   * compose_physical, use `add_rows_raw` instead.
   *
   * @param rows The sparse row representations of the constraints
   * @param eps  Epsilon value for coefficient filtering
   */
  void add_rows(std::span<const SparseRow> rows, double eps = 0.0);

  /**
   * @brief Bulk-add constraint rows in LP-raw space (skip col_scale +
   *        per-row row-max + scale_objective composition).
   *
   * Companion to `add_row_raw` but for batches.  Use when the caller
   * already has LP-raw coefficients/bounds and wants the bulk speedup
   * without compose_physical.  Typical use: snapshot replay paths that
   * carry LP-raw rows.
   *
   * Only `SparseRow::scale` is composed (mirroring `flatten()`'s
   * treatment of static rows).  No per-column col_scale, no per-row
   * equilibration, no scale_objective divisor.
   *
   * @param rows The sparse row representations of the constraints
   *             (LP-raw coefficients and LP-raw bounds).
   * @param eps  Epsilon value for coefficient filtering.
   */
  void add_rows_raw(std::span<const SparseRow> rows, double eps = 0.0);

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
   *
   * Under `LowMemoryMode::rebuild`, transparently triggers the rebuild
   * callback when the backend is still released so callers never read a
   * stale cached row count from an un-built cell.  `snapshot`/`compress`
   * modes pre-seed `m_cached_numrows_` via `defer_initial_load`, so
   * `get_numrows()` returns the correct value without a reconstruct —
   * preserving the invariant that save_base_numrows does not flip the
   * released flag for those modes.
   */
  void save_base_numrows()
  {
    if (m_low_memory_mode_ == LowMemoryMode::rebuild) {
      ensure_backend();
    }
    m_base_numrows_ = get_numrows();
    m_base_numrows_set_ = true;
    // Legacy callers reach the cut-build phase boundary here too.
    // Advance the lifecycle observer (step 2 of the lifecycle plan)
    // so the legacy three-call sequence and the new
    // `freeze_for_cuts` consolidator both leave the LP in `Frozen`.
    if (m_phase_ == LiPhase::Building) {
      m_phase_ = LiPhase::Frozen;
    }
  }

  /**
   * @brief Get the saved base row count.
   * @return Base row count (0 if save_base_numrows was never called)
   */
  [[nodiscard]] constexpr auto base_numrows() const noexcept
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
   * @return Number of rows (signed `Index` to match `LinearProblem`'s
   *         API and the solver backends — every solver C API uses `int`,
   *         and the strong-index `RowIndex` constructor takes `Index`).
   */
  [[nodiscard]] Index get_numrows() const;

  /**
   * @brief Gets the number of variable columns in the problem
   * @return Number of columns (see `get_numrows` rationale).
   */
  [[nodiscard]] Index get_numcols() const;

  /**
   * @brief Typed row count — use instead of `RowIndex{li.get_numrows()}`
   *        at every call site.
   *
   * `get_numrows()` already returns `Index`, so this just wraps the
   * strong-type ctor.  Matches the typed API used across the rest of
   * the LP layer (`add_row` → `RowIndex`, `delete_row` → `RowIndex`, …)
   * so idiomatic call sites stay inside strong-index space.
   */
  [[nodiscard]] RowIndex numrows_as_index() const
  {
    return RowIndex {get_numrows()};
  }

  /// See `numrows_as_index`.
  [[nodiscard]] ColIndex numcols_as_index() const
  {
    return ColIndex {get_numcols()};
  }

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
  [[nodiscard]] auto get_algorithm() const { return backend().get_algorithm(); }

  /// Currently configured thread count (0 = solver default).
  [[nodiscard]] auto get_threads() const { return backend().get_threads(); }

  /// Whether presolve is currently enabled.
  [[nodiscard]] auto get_presolve() const { return backend().get_presolve(); }

  /// Current solver log verbosity level (0 = off).
  [[nodiscard]] auto get_log_level() const { return backend().get_log_level(); }

  /// Solver's representation of +infinity for variable bounds.
  ///
  /// Safe to call even when the backend has been released (low-memory
  /// modes): the value is cached at ctor time and on every successful
  /// `load_flat`.  Used by the rebuild callback's `flatten_from_collections`
  /// pass, which needs infinity before the backend is repopulated.
  [[nodiscard]] double infinity() const noexcept
  {
    return m_backend_ ? m_backend_->infinity() : m_cached_infinity_;
  }

  /// Normalize a bound value: map gtopt::DblMax to the solver's infinity.
  ///
  /// SparseCol/SparseRow use `std::numeric_limits<double>::max()` as default
  /// unbounded markers, but solver backends (e.g. HiGHS) may use a smaller
  /// infinity threshold (e.g. 1e30).  This method translates at the
  /// LinearInterface boundary so that formulation code and solver agree.
  [[nodiscard]] double normalize_bound(double value) const noexcept
  {
    const auto inf = infinity();
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
    return value >= infinity();
  }

  /// True if @p value represents negative infinity for the active solver.
  [[nodiscard]] bool is_neg_inf(double value) const noexcept
  {
    return value <= -infinity();
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

  /**
   * @brief Sets the objective coefficient for a column in physical units.
   *
   * Composes the same physical → LP transform that `flatten()` /
   * `add_col(SparseCol)` apply to the cost field:
   *   raw = physical_value × col_scale[index] / scale_objective
   *
   * When `col_scale[index] == 1` and `scale_objective == 1` (the
   * common bare-LinearInterface / unflattened case), this is
   * equivalent to writing `physical_value` to the backend directly.
   *
   * @param index           Column index.
   * @param physical_value  Objective coefficient in physical units.
   */
  void set_obj_coeff(ColIndex index, double physical_value);

  /**
   * @brief Sets the objective coefficient for a column in LP-raw units.
   *
   * Writes `value` to the backend verbatim — no `col_scale` /
   * `scale_objective` composition.  Companion to the raw bound
   * setters (`set_col_low_raw`, `set_rhs_raw`) and the raw accessor
   * `get_obj_coeff()` (which returns the backend's raw obj vector).
   *
   * @param index Column index.
   * @param value LP-raw objective coefficient.
   */
  void set_obj_coeff_raw(ColIndex index, double value);

  [[nodiscard]] auto get_obj_coeff() const
  {
    return std::span(backend().obj_coefficients(), get_numcols());
  }

  // ── Column bound setters (raw: LP/solver units) ──

  void set_col_low_raw(ColIndex index, double value);
  void set_col_upp_raw(ColIndex index, double value);
  void set_col_raw(ColIndex index, double value);

  // ── Column bound setters (physical: descaled) ──

  void set_col_low(ColIndex index, double physical_value);
  void set_col_upp(ColIndex index, double physical_value);
  void set_col(ColIndex index, double physical_value);

  /**
   * @brief Gets the objective value in physical (unscaled) units.
   *
   * Returns `raw_obj × scale_objective`, converting from LP space back
   * to physical cost units.  Equivalent to the old manual descaling
   * `get_obj_value_raw() * options.scale_objective()`.
   * @return Physical objective value (post-descaling)
   */
  [[nodiscard]] double get_obj_value() const;

  /**
   * @brief Gets the raw LP objective value (in solver/LP space).
   *
   * Returns the value the LP solver reports directly, before
   * `scale_objective` is reversed.  Use `get_obj_value()` when callers
   * need physical / cost units; use this only for tests and diagnostics
   * that need to inspect the LP-side value the solver actually returned.
   * @return Raw LP objective value (pre-descaling)
   */
  [[nodiscard]] double get_obj_value_raw() const;

  /**
   * @brief Writes the problem to an LP format file
   * @param filename Name of the file to write (without extension)
   * @return Success, or an error if row names are not available
   */
  [[nodiscard]] std::expected<void, Error> write_lp(
      const std::string& filename) const;

  /**
   * @brief Produce `(col_names, row_names)` for the current LP by
   *        synthesising real gtopt labels on demand.
   *
   * Sources, in priority order:
   *   1. Pre-formatted strings in `m_col_index_to_name_` /
   *      `m_row_index_to_name_` (populated at flatten when
   *      `LpMatrixOptions::{col,row}_with_names` was set — i.e.
   *      `--lp-debug`).  Zero work if already present.
   *   2. Structural metadata: `m_col_labels_meta_` /
   *      `m_row_labels_meta_` (populated unconditionally at flatten).
   *      Formatted via a local `LabelMaker{LpNamesLevel::all}` so
   *      the result matches what `--lp-debug` would have produced
   *      at flatten time.
   *   3. Dynamic cols (post-flatten additions, e.g. α): synthesised
   *      from `m_dynamic_cols_[k]` via the same forced-all
   *      `LabelMaker`.
   *   4. Active cuts (post-flatten row additions): same, from
   *      `m_active_cuts_[k]`.
   *
   * No fallback — if a col/row has neither pre-formatted name nor
   * metadata, that indicates a `LinearInterface` invariant
   * violation (e.g. a col was added without metadata, or load_flat
   * was called with a FlatLinearProblem that pre-dates this
   * contract).  The method throws `std::logic_error` in that case.
   *
   * Caller pays zero memory cost at flatten; synthesis happens only
   * when this method (and therefore `write_lp`) actually fires.
   *
   * @param col_names  Output vector, resized to `get_num_cols()`.
   * @param row_names  Output vector, resized to `get_num_rows()`.
   */
  /// Caches formatted labels back into `m_col_index_to_name_` /
  /// `m_row_index_to_name_` so subsequent calls (repeat `write_lp`)
  /// skip the `LabelMaker` pass.  The caches are declared `mutable`
  /// so this method remains logically const.
  void generate_labels_from_maps(std::vector<std::string>& col_names,
                                 std::vector<std::string>& row_names) const;

  /// Force label synthesis into the internal caches + name→index maps
  /// without producing output vectors.  Useful for callers that
  /// subsequently read `row_name_map()` / `col_name_map()` or
  /// `row_index_to_name()` / `col_index_to_name()` and need them
  /// populated under the lazy add_col / add_row path.
  void materialize_labels() const;

private:
  /// Bulk-replay helper for `apply_post_load_replay`.  Wraps the
  /// general-purpose `add_rows(span)` call against `m_active_cuts_`
  /// so the cut-replay site reads as a verb rather than a generic
  /// bulk-add — matches plan step 6 of the lifecycle refactor's
  /// "make the single legitimate caller obvious" intent without
  /// breaking the public `add_rows(span)` test that exercises the
  /// cross-backend bulk dispatch.
  void replay_active_cuts();

  /// Deep-copy helper for `shared_ptr<T>` members.  Returns a fresh
  /// `shared_ptr` owning a value-copy of the source's underlying T —
  /// the deep-clone counterpart to the shallow-clone `shared_ptr`
  /// copy-assignment.  Lazily handles the null source-pointer case
  /// (returns a shared_ptr to a default-constructed T).
  template<class T>
  [[nodiscard]] static std::shared_ptr<T> deep_copy_ptr(
      const std::shared_ptr<T>& src)
  {
    return src ? std::make_shared<T>(*src) : std::make_shared<T>();
  }

  /// Copy-on-write helper for `shared_ptr<T>` members.  Returns a
  /// mutable reference to the underlying T, detaching first if the
  /// pointer is shared (`use_count() > 1`).  Lazily creates the T if
  /// the shared_ptr is null.
  ///
  /// Under the project's actual usage pattern this helper is always
  /// the no-op branch:
  ///
  ///   - When the source is mutating (cut adds between aperture
  ///     phases, α-col registration, post-load replay) every clone
  ///     has already been destroyed → `use_count == 1`, no copy.
  ///   - When clones are alive (aperture phase, elastic recovery)
  ///     the source is frozen by `DecompressionGuard`, and clones
  ///     use `add_*_disposable` which never touches shared state.
  ///
  /// The COW branch is therefore defensive only — guarantees
  /// correctness if a future caller violates the freeze contract,
  /// instead of producing a silent data race.
  template<class T>
  static T& detach_for_write(std::shared_ptr<T>& p)
  {
    if (!p) {
      p = std::make_shared<T>();
    } else if (p.use_count() > 1) {
      p = std::make_shared<T>(*p);
    }
    return *p;
  }

  /// Pure backend emit: append a column to the solver's matrix with
  /// normalised bounds and the obj-coeff already divided by
  /// `m_scale_objective_`.  No metadata tracking, no `set_col_scale`,
  /// no dedup.  Used as the shared core of `add_col(SparseCol)` and
  /// (indirectly) `add_col_raw`.  The disposable APIs deliberately do
  /// NOT route through this helper — they call `m_backend_->add_col`
  /// directly to keep their mutation surface trivially auditable.
  ColIndex emit_col_to_backend(const SparseCol& col);

  /// Bulk variant of `emit_col_to_backend`.  Assembles CSC buffers
  /// for the batch and dispatches a single `m_backend_->add_cols(...)`.
  /// Shared by `add_cols(span)` (physical) and `add_cols_raw` (raw)
  /// so neither path duplicates the buffer-building logic — mirrors
  /// the `emit_col_to_backend` ↔ `add_col` / `add_col_raw` shape.
  /// Returns the `ColIndex` of the FIRST added column; callers
  /// compute per-element indices as `first + c`.
  ColIndex emit_cols_to_backend(std::span<const SparseCol> cols);

  /// Pure backend emit: append a row to the solver's matrix.  Composes
  /// `SparseRow::scale` (mirroring `flatten()`'s static-row handling)
  /// but applies no `col_scale` multiplication and no per-row
  /// equilibration.  Used as the shared core of `add_row_raw`.  The
  /// disposable APIs do NOT route through this helper.
  RowIndex emit_row_to_backend(const SparseRow& row, double eps);

  /// Append/update the col-label metadata for a freshly-added column.
  /// Called by `add_col(SparseCol)` and `add_col_raw` after the col
  /// index is known.  Resizes `m_col_labels_meta_` so `m[col_idx]`
  /// carries the 4-tuple LabelMaker needs, and inserts into
  /// `m_col_meta_index_` for eager duplicate detection.
  ///
  /// NOTE: the dedup throw at the bottom of this function has three
  /// near-identical siblings (`track_row_label_meta`,
  /// `add_col_disposable`, `add_row_disposable`).  They differ in
  /// which dedup map(s) they consult and which "role" string the
  /// error message uses ("col" / "cons" / "shared col" /
  /// "first disposable").  Keep the wording in sync if you change one.
  void track_col_label_meta(ColIndex col_idx, const SparseCol& col);

  /// Append/update the row-label metadata for a freshly-added row.
  /// Called by the `add_row(SparseRow)` entry points after the row
  /// index is known.  Resizes `m_row_labels_meta_` so `m[row_idx]`
  /// carries the 4-tuple LabelMaker needs.
  void track_row_label_meta(RowIndex row_idx, const SparseRow& row);

  /// Compress the live `m_col_labels_meta_` / `m_row_labels_meta_`
  /// vectors into their `_compressed_` backups and clear the live
  /// copies.  Called from `release_backend` under
  /// `LowMemoryMode::compress`.  No-op in other modes.
  void compress_labels_meta_if_needed();

  /// Lazy-lazy decompression: if the live metadata is empty but a
  /// compressed buffer is non-empty, decompress it back into the
  /// live vector (and the string pool).  No-op otherwise.  Safe to
  /// call repeatedly and from const contexts (members are mutable).
  void ensure_labels_meta_decompressed() const;

  /// Rebuild `m_col_meta_index_` / `m_row_meta_index_` from the live
  /// metadata vectors.  Called after `load_flat` and after
  /// `ensure_labels_meta_decompressed` so the eager duplicate check
  /// in `add_col` / `track_row_label_meta` can run against the full
  /// history, not just post-reload additions.
  void rebuild_meta_indexes() const;

public:
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
   * @brief Gets the condition number of the basis matrix (if available).
   * @return Condition number kappa, or `std::nullopt` if the backend
   *         cannot compute one (e.g. no basis after barrier without
   *         crossover, backend does not expose the query, or the
   *         native API reported failure).
   *
   * Callers must NOT treat a missing value as 1.0 — that silently
   * poisons any `std::max`-based aggregation across a (scene, phase)
   * grid.  Use `std::optional::value_or`, the bool-convert, or explicit
   * `has_value()` checks instead.
   */
  [[nodiscard]] std::optional<double> get_kappa() const;

  /**
   * @brief Read-only access to this LP's cumulative solver counters.
   *
   * Counters are incremented by `load_flat`, `initial_solve`, `resolve`,
   * and `ensure_duals`.  Aggregation across LPs (e.g. for end-of-run
   * reporting) is done by the caller via `SolverStats::operator+=`.
   */
  [[nodiscard]] constexpr const SolverStats& solver_stats() const noexcept
  {
    return m_solver_stats_;
  }

  /**
   * @brief Mutable accessor to the solver-counter block.
   *
   * Exposed for out-of-class instrumentation — specifically
   * `SDDPMethod::backward_pass_single_phase`, which bumps the six
   * `bwd_*_s` timers plus `bwd_step_count` on the previous-phase LP as
   * it installs each optimality cut.  Callers are expected to be
   * single-writer threads for the underlying LP (same contract as every
   * other mutating method on `LinearInterface`).
   */
  [[nodiscard]] constexpr SolverStats& mutable_solver_stats() noexcept
  {
    return m_solver_stats_;
  }

  /**
   * @brief Fold another LP's counters into this one.
   *
   * Used by elastic-filter paths that resolve a `clone()`d LP and
   * discard it: call this on the original with `clone.solver_stats()`
   * before the clone is destroyed so the solve attempts it made still
   * show up in the final report.
   */
  constexpr void merge_solver_stats(const SolverStats& other) noexcept
  {
    m_solver_stats_ += other;
  }

  /**
   * @brief Read-only access to this LP's incremental validation stats.
   *
   * Stats are accumulated by every `add_col` / `add_row` / `set_*`
   * write that lands on the LP after `set_validation_options(...)` is
   * called.  The default-constructed `LpValidationOptions` has
   * `enable.value_or(true)` → tracking is on out of the box.
   */
  [[nodiscard]] constexpr const LpValidationStats& lp_validation_stats()
      const noexcept
  {
    return m_validation_stats_;
  }

  /**
   * @brief Mutable accessor to the validation stats block.
   */
  [[nodiscard]] constexpr LpValidationStats&
  mutable_lp_validation_stats() noexcept
  {
    return m_validation_stats_;
  }

  /**
   * @brief Read-only access to the per-LP validation thresholds.
   */
  [[nodiscard]] constexpr const LpValidationOptions& lp_validation_options()
      const noexcept
  {
    return m_validation_options_;
  }

  /**
   * @brief Install or replace this LP's validation thresholds.
   *
   * Typically wired up at construction time from
   * `LpMatrixOptions::validation`.  Mutates only `m_validation_options_`;
   * existing accumulator state is preserved.
   */
  constexpr void set_validation_options(LpValidationOptions opts) noexcept
  {
    m_validation_options_ = std::move(opts);
  }

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
    return std::span(backend().row_lower(), get_numrows());
  }

  /**
   * @brief Gets raw upper bounds for all constraint rows (LP units).
   * @return Span view of raw row upper bounds
   */
  [[nodiscard]] auto get_row_upp_raw() const
  {
    return std::span(backend().row_upper(), get_numrows());
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
  // `backend()` may call `ensure_backend()`, which can throw on a
  // rebuild failure — a programmer-bug path we intentionally allow to
  // terminate instead of unwinding partial LP state.  Keep `noexcept`
  // on the API surface and suppress tidy; NOLINT lines are narrower
  // than dropping the guarantee from every caller.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] ScaledView get_row_low() const noexcept
  {
    const auto n = get_numrows();
    return {backend().row_lower(),
            n,
            m_row_scales_->data(),
            m_row_scales_->size(),
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets physical upper bounds for all constraint rows.
   * @return ScaledView over solver row upper bounds
   */
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] ScaledView get_row_upp() const noexcept
  {
    const auto n = get_numrows();
    return {backend().row_upper(),
            n,
            m_row_scales_->data(),
            m_row_scales_->size(),
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
    return std::span(backend().col_lower(), get_numcols());
  }

  /**
   * @brief Gets raw upper bounds for all variable columns (LP units).
   * @return Span view of raw column upper bounds
   */
  [[nodiscard]] auto get_col_upp_raw() const
  {
    return std::span(backend().col_upper(), get_numcols());
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
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] ScaledView get_col_low() const noexcept
  {
    const auto n = get_numcols();
    return {backend().col_lower(),
            n,
            m_col_scales_->data(),
            m_col_scales_->size(),
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets physical upper bounds for all variable columns.
   *
   * Returns a zero-copy lazy view: each access computes
   * `LP_bound × col_scale` on the fly.
   * @return ScaledView over solver column upper bounds
   */
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] ScaledView get_col_upp() const noexcept
  {
    const auto n = get_numcols();
    return {backend().col_upper(),
            n,
            m_col_scales_->data(),
            m_col_scales_->size(),
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
    if (m_backend_released_ && !m_cached_col_sol_.empty()) {
      return {m_cached_col_sol_.data(), m_cached_col_sol_.size()};
    }
    return {backend().col_solution(), static_cast<size_t>(get_numcols())};
  }

  /**
   * @brief Gets physical solution values (LP × col_scale).
   *
   * Returns a zero-copy lazy view: each access computes
   * `LP_value × col_scale` on the fly.  When col_scales are empty,
   * returns raw values unchanged.
   *
   * When the last solve converged to optimality (`is_optimal()` true),
   * the returned view additionally clamps each element to the column's
   * physical bound box `[col_low[i], col_upp[i]]`.  The clamp is
   * applied *after* descaling so no further scale multiplication can
   * re-introduce the bound violation.  This scrubs solver-tolerance
   * noise that would otherwise propagate into subsequent phases as
   * `set_col_low_raw(dep, sol+eps) = set_col_upp_raw(dep, sol+eps)`
   * and make the next-phase LP trivially infeasible.  Callers that
   * need the *exact* solver output (e.g. Chinneck IIS on an elastic
   * clone where relaxed bounds would falsely "fix" the value) use
   * `get_col_sol_raw()`.
   *
   * Precondition: backend must be live.  Callers under low-memory
   * modes should invoke `ensure_backend()` first if reading after a
   * release.
   * @return ScaledView over solver solution memory
   */
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] ScaledView get_col_sol() const noexcept
  {
    const auto n = get_numcols();
    const double* data = (m_backend_released_ && !m_cached_col_sol_.empty())
        ? m_cached_col_sol_.data()
        : backend().col_solution();
    // Clamp path requires a live backend: col bounds are not cached on
    // `release_backend`, so we'd dereference a null `m_backend_` to
    // read them.  Also skip when the last solve wasn't optimal —
    // solver values may then be arbitrary and clamping is misleading.
    if (m_backend_released_ || !m_cached_is_optimal_) {
      return {data,
              n,
              m_col_scales_->data(),
              m_col_scales_->size(),
              ScaledView::Op::multiply};
    }
    return {data,
            n,
            m_col_scales_->data(),
            m_col_scales_->size(),
            backend().col_lower(),
            n,
            backend().col_upper(),
            n,
            ScaledView::Op::multiply};
  }

  /**
   * @brief Gets raw LP reduced costs (no descaling).
   * @return Span view of raw LP reduced costs
   */
  [[nodiscard]] auto get_col_cost_raw() const -> std::span<const double>
  {
    if (m_backend_released_ && !m_cached_col_cost_.empty()) {
      return {m_cached_col_cost_.data(), m_cached_col_cost_.size()};
    }
    return {backend().reduced_cost(), static_cast<size_t>(get_numcols())};
  }

  /**
   * @brief Gets LP-space reduced costs scaled by `scale_objective / col_scale`.
   *
   * **Important**: despite the historical name, this does NOT return
   * truly physical reduced costs in $/[physical-unit]/hour.  The result
   * is `rc_LP × scale_objective / col_scale`, which unscales:
   *   - the global `scale_objective` divisor;
   *   - the per-column `col_scale` (user `variable_scales` and
   *     equilibration adjustments).
   *
   * It does NOT unscale the per-(scenario, stage, block) `cost_factor
   * = probability × discount × duration` that's folded into the LP cost
   * coefficients via `CostHelper::block_ecost / stage_ecost`.  Callers
   * who need reduced costs in physical $/[unit]/hour must additionally
   * divide by `CostHelper::cost_factor(scenario, stage[, block])` for
   * the column's context.
   *
   * `OutputContext::add_col_cost` already applies the inverse factor
   * to deliver truly physical reduced costs to the user-facing output
   * stream; SDDP cut math intentionally consumes the LP-folded value
   * here and relies on cancellation in the destination master LP.
   *
   * Precondition: backend must be live.
   * @return Zero-copy lazy view per element (LP-space rc, cost_factor-folded).
   */
  // NOLINTNEXTLINE(bugprone-exception-escape)
  [[nodiscard]] ScaledView get_col_cost() const noexcept
  {
    const auto n = get_numcols();
    if (m_backend_released_ && !m_cached_col_cost_.empty()) {
      return {m_cached_col_cost_.data(),
              n,
              m_col_scales_->data(),
              m_col_scales_->size(),
              ScaledView::Op::divide,
              m_scale_objective_};
    }
    return {backend().reduced_cost(),
            n,
            m_col_scales_->data(),
            m_col_scales_->size(),
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
  [[nodiscard]] double get_col_scale(ColIndex index) const noexcept
  {
    if (static_cast<size_t>(index) < m_col_scales_->size()) {
      return (*m_col_scales_)[index];
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
  void set_col_scale(ColIndex index, double scale)
  {
    auto& cs = detach_for_write(m_col_scales_);
    const auto sz = static_cast<size_t>(index) + 1;
    if (sz > cs.size()) {
      cs.resize(sz, 1.0);
    }
    cs[index] = scale;
  }

  /**
   * @brief Gets all column scale factors.
   * @return Const reference to the column scale vector (empty if not
   * populated)
   */
  [[nodiscard]] const auto& get_col_scales() const noexcept
  {
    return *m_col_scales_;
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
  [[nodiscard]] double get_row_scale(RowIndex index) const noexcept
  {
    if (static_cast<size_t>(index) < m_row_scales_->size()) {
      return (*m_row_scales_)[index];
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
  void set_row_scale(RowIndex index, double scale)
  {
    auto& rs = detach_for_write(m_row_scales_);
    const auto sz = static_cast<size_t>(index) + 1;
    if (sz > rs.size()) {
      rs.resize(sz, 1.0);
    }
    rs[index] = scale;
  }

  /**
   * @brief Gets all row equilibration scale factors.
   * @return Const reference to the row scale vector (empty if not populated)
   */
  [[nodiscard]] const auto& get_row_scales() const noexcept
  {
    return *m_row_scales_;
  }

  /// Equilibration method in effect for this LP (selected by
  /// `opts.equilibration_method` at `flatten()` time, persisted across
  /// `load_flat`).  Used by `add_row` / `add_rows` to keep applying
  /// per-row scaling to post-build cuts.  `none` when equilibration
  /// was disabled at build time — in that case `add_row` leaves new
  /// rows alone, preserving the historical behaviour.
  [[nodiscard]] constexpr LpEquilibrationMethod equilibration_method()
      const noexcept
  {
    return m_equilibration_method_;
  }

  /// Override the equilibration method recorded for this LP.  Normally
  /// set by `load_flat` from `FlatLinearProblem::equilibration_method`;
  /// this setter exists for unit tests that build an LP directly via
  /// `add_col` / `add_row` (no flatten pass) and still want to exercise
  /// the `add_equilibrated_row` path.
  void set_equilibration_method(LpEquilibrationMethod method) noexcept
  {
    m_equilibration_method_ = method;
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
    return *m_variable_scale_map_;
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
    if (m_backend_released_ && !m_cached_row_dual_.empty()) {
      return {m_cached_row_dual_.data(), m_cached_row_dual_.size()};
    }
    ensure_duals();
    return {backend().row_price(), static_cast<size_t>(get_numrows())};
  }

  /**
   * @brief Gets LP-space dual values, scaled by `scale_objective / row_scale`.
   *
   * **Important**: despite the historical name, this does NOT return
   * truly physical shadow prices in $/[physical-unit]/hour.  The result
   * is `raw_dual × scale_objective / composite_row_scale`, which
   * unscales:
   *   - the global `scale_objective` divisor applied at flatten;
   *   - row-max equilibration (`max|coeff|`);
   *   - any explicit `SparseRow::scale` (used for column-side
   *     neutralization, e.g. cancelling a state-variable column's
   *     `var_scale`).
   *
   * It does NOT unscale the per-(scenario, stage, block) `cost_factor
   * = probability × discount × duration` that's folded into the LP cost
   * coefficients via `CostHelper::block_ecost / stage_ecost`.  Callers
   * who need shadow prices in physical $/[unit]/hour must additionally
   * divide by `CostHelper::cost_factor(scenario, stage[, block])`.
   *
   * Two production paths already do this:
   *   - `OutputContext::add_row_dual` (block-level rows): multiplies by
   *     `block_icost_factors() = 1 / cost_factor(s, t, b)`.
   *   - `OutputContext::add_row_dual` (stage-level rows): multiplies by
   *     `scenario_stage_icost_factors() = 1 / cost_factor(s, t)`.
   *
   * Internal SDDP cut math intentionally consumes the LP-folded value
   * here and relies on the same factor being present in the destination
   * master LP (cancellation).  See `sddp_cut_sharing.cpp:88-122` and
   * `benders_cut.cpp` for the cut convention.
   *
   * Precondition: backend must be live.
   * @return Zero-copy lazy view per element (LP-space dual,
   *         cost_factor-folded).
   */
  [[nodiscard]] ScaledView get_row_dual()
  {
    const auto n = get_numrows();
    if (m_backend_released_ && !m_cached_row_dual_.empty()) {
      return {m_cached_row_dual_.data(),
              n,
              m_row_scales_->data(),
              m_row_scales_->size(),
              ScaledView::Op::divide,
              m_scale_objective_};
    }
    ensure_duals();
    return {backend().row_price(),
            n,
            m_row_scales_->data(),
            m_row_scales_->size(),
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

  /// Column (variable) name → strong column index map.
  using col_name_map_t = std::unordered_map<std::string, ColIndex>;
  /// Row (constraint) name → strong row index map.
  using row_name_map_t = std::unordered_map<std::string, RowIndex>;

  [[nodiscard]] const row_name_map_t& row_name_map() const noexcept
  {
    return *m_row_names_;
  }

  [[nodiscard]] const col_name_map_t& col_name_map() const noexcept
  {
    return *m_col_names_;
  }

  /// Column index → name vector (empty string for unnamed columns).
  /// Populated alongside col_name_map when names are enabled.
  [[nodiscard]] const auto& col_index_to_name() const noexcept
  {
    return *m_col_index_to_name_;
  }

  /// Row index → name vector (empty string for unnamed rows).
  /// Populated alongside row_name_map when names are enabled.
  [[nodiscard]] const auto& row_index_to_name() const noexcept
  {
    return *m_row_index_to_name_;
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
  [[nodiscard]] constexpr std::optional<ColIndex> lp_stats_max_col()
      const noexcept
  {
    return m_stats_max_col_;
  }
  [[nodiscard]] constexpr std::optional<ColIndex> lp_stats_min_col()
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
      interface->backend().set_log_filename(filename, level);
    }

    // `backend().clear_log_filename()` delegates to `ensure_backend()`
    // + `SolverBackend::clear_log_filename`; both can theoretically
    // throw on a rebuild/solver-specific failure.  We intentionally
    // keep the implicit-noexcept destructor — a failure here is a
    // programmer bug that should terminate rather than unwind.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~LogFileGuard() { interface->backend().clear_log_filename(); }
  };

  void open_log_handler(int log_level);
  void close_log_handler();

  /// Centralized, auto-resurrecting access to the solver backend.
  ///
  /// Every read path that dereferences the backend should funnel through
  /// here so callers don't have to remember `ensure_backend()` /
  /// `SystemLP::ensure_lp_built()` before each access.  Under
  /// `low_memory_mode != off` the backend may be released between
  /// operations (e.g. by the SDDP forward pass after capturing the
  /// solution); this accessor reconstructs it on first access and
  /// returns a live reference.
  ///
  /// The `const` overload uses `const_cast` to invoke the non-const
  /// `ensure_backend()` — standard lazy-init idiom, since rebuilding
  /// cached backend state is a logically-const cache-refill.
  [[nodiscard]] SolverBackend& backend()
  {
    ensure_backend();
    return *m_backend_;
  }
  [[nodiscard]] const SolverBackend& backend() const
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    const_cast<LinearInterface*>(this)->ensure_backend();
    return *m_backend_;
  }

  std::unique_ptr<SolverBackend> m_backend_;
  std::string m_solver_name_ {};  ///< Solver name for backend reconstruction
  std::string m_solver_version_ {};  ///< Cached version for released backends

  /// Cached +infinity value queried from the backend whenever it is
  /// live (ctor and `load_flat`).  Used by `infinity()` /
  /// `normalize_bound()` when the backend is released — in particular
  /// by the rebuild callback's flatten pass, which must normalise
  /// DblMax bounds *before* the backend is repopulated.
  double m_cached_infinity_ {DblMax};
  SolverOptions m_last_solver_options_ {};  ///< Options from last solve
  std::string m_log_file_ {};
  std::string m_log_tag_ {};  ///< Context tag prefixed to fallback warnings
  LabelMaker m_label_maker_ {};  ///< Label generator + level gate

  /// Name-to-index maps for duplicate detection and later lookup.
  /// Populated when names are enabled.
  // Mutable for the lazy-materialisation path: caches populated by
  // `generate_labels_from_maps` (logically const) live here too.
  mutable std::shared_ptr<row_name_map_t> m_row_names_ {
      std::make_shared<row_name_map_t>()};  ///< Row (constraint) name → idx
  mutable std::shared_ptr<col_name_map_t> m_col_names_ {
      std::make_shared<col_name_map_t>()};  ///< Column (variable) name → idx
  // Mutable so `generate_labels_from_maps` (logically const — it
  // returns new vectors; the state update is a caching detail) can
  // persist freshly-formatted labels for reuse on subsequent calls.
  mutable std::shared_ptr<StrongIndexVector<ColIndex, std::string>>
      m_col_index_to_name_ {
          std::make_shared<StrongIndexVector<ColIndex, std::string>>()};
  mutable std::shared_ptr<StrongIndexVector<RowIndex, std::string>>
      m_row_index_to_name_ {
          std::make_shared<StrongIndexVector<RowIndex, std::string>>()};

  Index m_base_numrows_ {};  ///< Row count before any cuts were added
  /// True once `save_base_numrows()` has fired.  Distinct from
  /// `m_base_numrows_ > 0` because the structural build may legitimately
  /// end at zero rows (e.g. pure column LPs, or tests that call
  /// save_base_numrows before any rows exist).  `add_row` uses this to
  /// decide whether incoming rows are cut-phase (physical + equilibration)
  /// or structural-build-phase (LP-space pass-through).
  bool m_base_numrows_set_ {false};

  double m_scale_objective_ {1.0};  ///< Global objective divisor (from flatten)
  /// Column / row scale vectors.  `shared_ptr` so shallow clones
  /// can share with the source — see `CloneKind`.  The scale vectors
  /// are populated in `load_flat` and only mutated post-flatten by
  /// `set_col_scale` / `set_row_scale` (called when a non-unit
  /// `col.scale` / `row.scale` is added via `add_col(SparseCol)` /
  /// `add_row_raw`).  Disposable adds explicitly forbid non-unit
  /// scales (see `add_col_disposable` / `add_row_disposable`) so
  /// they never trigger the COW detach branch on the clone side.
  mutable std::shared_ptr<StrongIndexVector<ColIndex, double>> m_col_scales_ {
      std::make_shared<StrongIndexVector<ColIndex, double>>()};
  mutable std::shared_ptr<StrongIndexVector<RowIndex, double>> m_row_scales_ {
      std::make_shared<StrongIndexVector<RowIndex, double>>()};
  /// Equilibration method used at load_flat() time.  Persisted so that
  /// `add_row` / `add_rows` (the post-build cut path) apply the same
  /// per-row scaling the bulk build did, keeping kappa stable as cuts
  /// accumulate.  `none` means the caller opted out of equilibration
  /// at build time and we leave new rows alone.
  LpEquilibrationMethod m_equilibration_method_ {LpEquilibrationMethod::none};
  /// Moved from flatten.  `shared_ptr` so shallow clones can share
  /// it with the source instead of value-copying — see `CloneKind`.
  /// Mutated only via `load_flat` (source-side, before any clones)
  /// so the COW detach in `detach_for_write` is dormant in practice.
  mutable std::shared_ptr<VariableScaleMap> m_variable_scale_map_ {
      std::make_shared<VariableScaleMap>()};

  /// Warm column solution loaded from a previous run's state file.
  /// Used by StorageLP::physical_eini/efin as fallback when
  /// !is_optimal() (hot start before first solve).  Empty if no
  /// state was loaded.
  StrongIndexVector<ColIndex, double> m_warm_col_sol_;

  size_t m_stats_nnz_ {};
  size_t m_stats_zeroed_ {};
  double m_stats_max_abs_ {};
  double m_stats_min_abs_ {};
  std::optional<ColIndex> m_stats_max_col_ {};
  std::optional<ColIndex> m_stats_min_col_ {};
  std::string m_stats_max_col_name_ {};
  std::string m_stats_min_col_name_ {};
  std::vector<FlatLinearProblem::RowTypeStatsEntry> m_row_type_stats_ {};

  struct FILEcloser
  {
    void operator()(FILE* f) const noexcept { std::fclose(f); }
  };
  using log_file_ptr_t = std::unique_ptr<FILE, FILEcloser>;
  log_file_ptr_t m_log_file_ptr_;

  /// Cache post-solve state and auto-release backend if low_memory is on.
  void cache_and_release();

  /// Call `m_rebuild_owner_->rebuild_in_place()`.  Defined in
  /// linear_interface.cpp where `SystemLP` is a complete type.
  void invoke_rebuild_owner();

  // ── Low-memory state ──────────────────────────────────────────────────

  LowMemoryMode m_low_memory_mode_ {LowMemoryMode::off};
  CompressionCodec m_memory_codec_ {CompressionCodec::lz4};

  /// Snapshot: flat LP (uncompressed or compressed), used by the
  /// `snapshot` / `compress` reconstruct path.  Always empty under
  /// `rebuild` — the flat LP is regenerated from collections instead.
  LowMemorySnapshot m_snapshot_ {};

  /// True while `apply_post_load_replay` is bulk-replaying
  /// `m_dynamic_cols_` / `m_dynamic_rows_` / `m_active_cuts_` onto the
  /// freshly loaded backend.  Used by the auto-record path in
  /// `add_col(SparseCol)` / `add_row(SparseRow)` to skip recording on
  /// the replay re-entry — without it, every reconstruct would push the
  /// same SparseCol back into `m_dynamic_cols_`, growing it unboundedly.
  /// The bulk variants `add_cols(span)` / `add_rows(span)` used by replay
  /// also bypass the single-arg path, so this flag is a defence-in-depth
  /// guard rather than the sole serialiser.
  bool m_replaying_ {false};

  /// Columns added after initial load_flat() (typically just alpha).
  std::vector<SparseCol> m_dynamic_cols_ {};

  /// Structural rows added after the initial flatten/load_flat (typically
  /// cascade elastic-target constraints).  Distinct from `m_active_cuts_`
  /// (SDDP optimality / feasibility cuts whose row count is mutated by
  /// the cut store across iterations) — these are FIXED rows that exist
  /// for the whole solve and must be replayed before `save_base_numrows`
  /// so the structural-vs-cut boundary correctly counts them on the
  /// "structural" side.
  std::vector<SparseRow> m_dynamic_rows_ {};

  /// Net active Benders cuts (additions minus deletions).
  std::vector<SparseRow> m_active_cuts_ {};

  /// Label-only metadata for the **frozen** flatten-time portion of the
  /// LP — set ONCE by `load_flat` from `flat_lp.col_labels_meta` /
  /// `row_labels_meta`, then never resized.  Indexed by the structural
  /// `[0, flatten_col_count())` / `[0, flatten_row_count())` half of
  /// the LP.  Post-flatten additions (cuts, slacks, alpha, cascade
  /// elastic constraints) live in `m_post_flatten_col_labels_meta_` /
  /// `m_post_flatten_row_labels_meta_` instead — see those fields and
  /// `col_label_at(ColIndex)` / `row_label_at(RowIndex)` for lookup.
  ///
  /// Sharing model:
  ///   * `shared_ptr<T>` so `clone(CloneKind::shallow)` hands the
  ///     vector out to aperture clones via atomic incref — zero copy.
  ///   * Because the vector is never resized post-`load_flat`, no
  ///     mutating site detaches it; clones and source share the same
  ///     storage forever (no copy-on-write churn on the first
  ///     post-flatten add).
  ///
  /// Under `LowMemoryMode::compress`, `release_backend` compresses
  /// these vectors into `m_col_labels_meta_compressed_` /
  /// `m_row_labels_meta_compressed_` and clears the live copies.
  /// Decompression is lazy-strict: it fires ONLY when
  /// `generate_labels_from_maps` (the `write_lp` consumer) actually
  /// needs to format strings — training / SDDP / simulation do not
  /// touch the decompressed form.  The decompressed strings live in
  /// `m_label_string_pool_` — the pool is never cleared while
  /// `m_col_labels_meta_` references it.  `mutable` because the lazy
  /// decompression flow is triggered from const methods.
  mutable std::shared_ptr<std::vector<SparseColLabel>> m_col_labels_meta_ {
      std::make_shared<std::vector<SparseColLabel>>()};
  mutable std::shared_ptr<std::vector<SparseRowLabel>> m_row_labels_meta_ {
      std::make_shared<std::vector<SparseRowLabel>>()};

  /// Label-only metadata for the **post-flatten** portion of the LP —
  /// extended by every `add_col(SparseCol)` / `add_row(SparseRow)`
  /// that runs after `load_flat` has installed the structural matrix.
  /// Hosts cut rows, alpha column, cascade elastic-target slacks +
  /// constraints, and any other dynamic addition.
  ///
  /// Per-instance — never wrapped in `shared_ptr`, never shared with
  /// clones.  A freshly-cloned `LinearInterface` starts with empty
  /// post-flatten vectors regardless of the source's history; the
  /// clone's own post-flatten additions land here independently of
  /// the source's, which is the correct semantics for aperture clones
  /// (each clone's elastic-filter slacks belong only to that clone).
  ///
  /// Lookup by index uses `col_label_at(ColIndex)` /
  /// `row_label_at(RowIndex)`: indices in `[0, flatten_col_count())`
  /// resolve against the frozen `m_col_labels_meta_`; indices in
  /// `[flatten_col_count(), flatten_col_count() +
  /// m_post_flatten_col_labels_meta_.size())` resolve against the
  /// post-flatten vector with the offset subtracted.
  ///
  /// Not compressed: the post-flatten vector is small in practice
  /// (alpha + a bounded set of cut rows / cascade elastic slacks) and
  /// is mutated frequently — round-tripping it through the codec on
  /// every release/reconstruct cycle would dominate the work that
  /// flatten-side compression saves.  The frozen flatten-side vector
  /// is the only one large enough to justify codec round-trips.
  std::vector<SparseColLabel> m_post_flatten_col_labels_meta_ {};
  std::vector<SparseRowLabel> m_post_flatten_row_labels_meta_ {};

  /// Eager dedup index for post-flatten metadata.  Mirrors the role of
  /// `m_col_meta_index_` (which now spans only the frozen flatten
  /// portion), but for the per-instance post-flatten additions.  Each
  /// post-flatten `add_col` / `add_row` consults BOTH maps to detect
  /// duplicates against either the frozen flatten metadata or any
  /// previously inserted post-flatten entry on this instance.
  std::unordered_map<SparseColLabel, ColIndex, SparseColLabelHash>
      m_post_flatten_col_meta_index_ {};
  std::unordered_map<SparseRowLabel, RowIndex, SparseRowLabelHash>
      m_post_flatten_row_meta_index_ {};

  /// Compressed backups of the metadata vectors — populated on
  /// `release_backend` under `compress` mode, drained on the first
  /// label-metadata read after reload.
  ///
  /// Intentionally NOT `shared_ptr`-wrapped: compression / decompression
  /// only ever runs on the source LP (via `release_backend` and
  /// `ensure_backend`); clones never compress.  The
  /// `DecompressionGuard` around the aperture pass ensures the source
  /// is in the decompressed state for the lifetime of any shallow
  /// clone, so the source uniquely owns these buffers throughout.
  mutable CompressedBuffer m_col_labels_meta_compressed_ {};
  mutable CompressedBuffer m_row_labels_meta_compressed_ {};
  mutable std::size_t m_col_labels_meta_count_ {0};
  mutable std::size_t m_row_labels_meta_count_ {0};

  /// Stable string storage backing decompressed `string_view`s in
  /// `m_col_labels_meta_` / `m_row_labels_meta_`.  Reserved ahead of
  /// decompression so `push_back` doesn't invalidate the views.
  mutable std::vector<std::string> m_label_string_pool_ {};

  /// Eager duplicate-detection maps, keyed on the (class_name,
  /// variable_name/constraint_name, variable_uid, context) metadata.
  /// Populated from `m_col_labels_meta_` / `m_row_labels_meta_` at
  /// `load_flat` time and on every `add_col` / `track_row_label_meta`
  /// call.  Cleared on `compress_labels_meta_if_needed` alongside the
  /// vectors and rebuilt on `ensure_labels_meta_decompressed`.
  ///
  /// These maps are the single source of truth for col/row uniqueness
  /// after the switch to metadata-driven label formatting — the
  /// name-based check in `LinearProblem::flatten` only runs when
  /// `col_with_name_map` / `row_with_name_map` is enabled (i.e.
  /// `--lp-debug`), which is off in production runs.  Keeping the
  /// check at `LinearInterface` level also catches dynamic
  /// post-flatten insertions (α column, Benders cut rows) that
  /// `LinearProblem` never sees.
  ///
  /// `shared_ptr<T>` so shallow clones share via atomic incref;
  /// mutating sites use `detach_for_write`.  `mutable` because
  /// `rebuild_meta_indexes` fires from the const
  /// `ensure_labels_meta_decompressed` path.
  mutable std::shared_ptr<
      std::unordered_map<SparseColLabel, ColIndex, SparseColLabelHash>>
      m_col_meta_index_ {std::make_shared<
          std::unordered_map<SparseColLabel, ColIndex, SparseColLabelHash>>()};
  mutable std::shared_ptr<
      std::unordered_map<SparseRowLabel, RowIndex, SparseRowLabelHash>>
      m_row_meta_index_ {std::make_shared<
          std::unordered_map<SparseRowLabel, RowIndex, SparseRowLabelHash>>()};

  /// Per-clone-local label metadata for cols/rows added via
  /// `add_col_disposable` / `add_row_disposable` after a shallow
  /// clone.  Empty on the source LP and on freshly-constructed clones;
  /// populated by the elastic-filter slack/fixing-row inserts on the
  /// cloned LP.
  ///
  /// Mirror of the production path: `m_post_clone_*_metas_` is
  /// indexed-by-insertion order (SparseColLabel / SparseRowLabel,
  /// same shape as `m_col_labels_meta_` / `m_row_labels_meta_`),
  /// while `m_post_clone_*_meta_index_` is the dedup hash map (same
  /// role as `m_col_meta_index_` / `m_row_meta_index_`) — duplicate
  /// insertions throw with both indices, just like the production
  /// path.
  ///
  /// `generate_labels_from_maps` consults these vectors when a col
  /// or row index is past the end of the shared metadata, so
  /// `write_lp` produces gtopt-formatted labels for disposable cols
  /// identical to what the production path would emit.
  mutable std::vector<std::pair<ColIndex, SparseColLabel>>
      m_post_clone_col_metas_ {};
  mutable std::vector<std::pair<RowIndex, SparseRowLabel>>
      m_post_clone_row_metas_ {};
  mutable std::unordered_map<SparseColLabel, ColIndex, SparseColLabelHash>
      m_post_clone_col_meta_index_ {};
  mutable std::unordered_map<SparseRowLabel, RowIndex, SparseRowLabelHash>
      m_post_clone_row_meta_index_ {};

  /// Whether the backend is currently released.
  bool m_backend_released_ {false};

  /// Lifecycle phase observer (step 2 of the lifecycle plan).
  /// Default-constructed in `Building`; advances through
  /// `Frozen`, `BackendReleased`, `Reconstructed` as the canonical
  /// entry points fire.  Pure observation; the plan's "step 4"
  /// addition of debug-asserted transitions stays deferred —
  /// existing tests bypass the canonical entry points so an
  /// assertion would fire on legitimate paths.
  LiPhase m_phase_ {LiPhase::Building};

  /// Re-entry guard for rebuild.  `ensure_backend()` sets this true
  /// before calling `m_rebuild_owner_->rebuild_in_place()`, which
  /// internally calls `load_flat` / `add_col` / `add_rows` that recurse
  /// through `ensure_backend`.  With the guard set those recursive calls
  /// early-return, avoiding an infinite loop.
  bool m_rebuilding_ {false};

  /// Back-pointer to the owning `SystemLP` under `LowMemoryMode::rebuild`.
  /// When non-null, `ensure_backend()` calls
  /// `m_rebuild_owner_->rebuild_in_place()` instead of the snapshot
  /// reconstruct path.  Set by `SystemLP::install_rebuild_callback` and
  /// re-set after every SystemLP move.  Never owns the pointee.
  SystemLP* m_rebuild_owner_ {};

  // ── Cached post-solve scalars (valid when backend is released) ─────

  /// Cached objective value from last successful solve.
  double m_cached_obj_value_ {};
  /// Cached kappa (condition number) from last successful solve.
  /// `std::nullopt` means "backend reported unavailable" — never silently
  /// reinterpret as 1.0.
  std::optional<double> m_cached_kappa_ {};
  /// Cached number of rows at time of release.
  Index m_cached_numrows_ {};
  /// Cached number of columns at time of release.
  Index m_cached_numcols_ {};
  /// Whether the cached state represents an optimal solution.
  bool m_cached_is_optimal_ {false};

  /// Cached post-solve primal/dual vectors (valid when `m_backend_released_`
  /// is true AND `m_cached_is_optimal_` is true).  Populated by
  /// `release_backend()` so downstream readers — `OutputContext`, Benders
  /// cut assembly, SDDP state propagation — can continue to access the
  /// solution after the solver backend has been dropped, without paying
  /// for a re-solve.  Empty when the backend was never released, never
  /// solved to optimum, or `m_low_memory_mode_ == LowMemoryMode::off`.
  std::vector<double> m_cached_col_sol_ {};
  std::vector<double> m_cached_col_cost_ {};
  std::vector<double> m_cached_row_dual_ {};

  /// Cumulative solver-activity counters (see `solver_stats.hpp`).
  /// Written only by the thread that owns this LP; aggregated across
  /// interfaces on the main thread at end of run.
  SolverStats m_solver_stats_ {};

  /// Per-LP build-time validation thresholds.  Defaults installed at
  /// construction (see `LpValidationOptions`); replaced at `load_flat`
  /// time by the matching field in `LpMatrixOptions::validation` once
  /// `SystemLP` plumbs it through, or directly via
  /// `set_validation_options(...)`.
  LpValidationOptions m_validation_options_ {};

  /// Accumulator for incremental validation events.  Hooked into every
  /// `add_col` / `add_row` / `set_*` write path; emits one
  /// `spdlog::warn` per offending value (capped per kind), and is
  /// available for end-of-build summarisation via
  /// `LpValidationStats::log_summary`.
  LpValidationStats m_validation_stats_ {};
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
///     auto c1 = li.clone();
///     auto c2 = li.clone();
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
