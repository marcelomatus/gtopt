/**
 * @file      lp_cache.hpp
 * @brief     LpCache — post-solve scalar + vector cache for LinearInterface
 * @date      2026-05-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `LpCache` owns the 9 post-solve cache fields that previously lived
 * directly on `LinearInterface` as `m_cached_*_`.  Extraction is
 * Phase 1 of the `LinearInterface` split (B2) — see
 * `docs/linear_interface_split_plan.md` §3 for the contract.
 *
 * Lifecycle:
 *   * Default-constructed: all empty / zero / nullopt / false.
 *   * After a successful solve: scalars (`obj_value`, `kappa`,
 *     `numrows`, `numcols`) are set, `is_optimal` is true,
 *     `backend_solution_fresh` is true, and the three vectors hold
 *     the post-solve primal / dual values (compress mode only —
 *     under `LowMemoryMode::off` the live backend is the source of
 *     truth and the vectors stay empty).
 *   * On structural mutation: `invalidate_optimal_on_mutation()`
 *     flips `is_optimal` to false; the vectors are not touched
 *     (callers may still read them via the empty-fallback path used
 *     by cut-construction code after backend release).
 *   * On backend reconstruct: `mark_solution_fresh(false)` —
 *     scalars + vectors stay; only the "live backend has fresh
 *     solution" gate is cleared until the next `resolve()` lands.
 *
 * Invariants (asserted in debug, see `validate_consistency()`):
 *   * **C1.**  `is_optimal()` ⇒ `col_sol().size() == numcols()`.
 *   * **C2.**  `invalidate_optimal_on_mutation()` is idempotent and
 *              does NOT clear the col_sol / col_cost / row_dual
 *              vectors.
 *   * **C3.**  `drop_solution_caches()` clears `col_sol` +
 *              `col_cost`, leaves `row_dual` and the scalars intact.
 *   * **C4.**  `set_is_optimal(true)` is the only path that flips
 *              the optimality flag to true.
 *   * **C5.**  `mark_solution_fresh(true)` fires inside `timed_solve`
 *              before the optimality check; `mark_solution_fresh(false)`
 *              fires on every `reconstruct_backend()` and
 *              `install_flat_as_rebuild()`.
 *   * **C6.**  `size_bytes()` reports the total bytes held by the
 *              three vectors.
 *   * **C7.**  `col_sol_buffer(n)` / `col_cost_buffer(n)` /
 *              `row_dual_buffer(n)` `resize` the underlying vector
 *              and return a span over it — the only write entry-
 *              point that backends use via `SolverBackend::fill_*`.
 *   * **C8.**  Default-constructed `LpCache{}` reports
 *              `is_optimal() == false`, `obj_value() == 0.0`,
 *              `numrows() == 0`, `numcols() == 0`,
 *              `kappa() == std::nullopt`, all spans empty.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * @brief Post-solve cache for LinearInterface.
 *
 * Owns the 9 scalar / vector fields that hold the most recent
 * successful solve's outputs (objective value, kappa, row/col
 * counts, optimality flag, primal/dual vectors).  Used by
 * downstream readers — `OutputContext`, Benders cut assembly, SDDP
 * state propagation — to access the solution after the solver
 * backend has been released under `LowMemoryMode::compress` /
 * `rebuild`, without paying for a re-solve.
 *
 * Empty when the backend was never released, never solved to
 * optimum, or `LowMemoryMode::off` (in which case the live
 * backend is the canonical source of truth and the LI getters
 * read directly from `SolverBackend::col_solution()` etc.).
 */
class LpCache
{
public:
  // ── Read accessors ──────────────────────────────────────────────────────

  [[nodiscard]] constexpr auto obj_value() const noexcept -> double
  {
    return m_obj_value_;
  }
  [[nodiscard]] constexpr auto kappa() const noexcept
      -> const std::optional<double>&
  {
    return m_kappa_;
  }
  [[nodiscard]] constexpr auto numrows() const noexcept -> Index
  {
    return m_numrows_;
  }
  [[nodiscard]] constexpr auto numcols() const noexcept -> Index
  {
    return m_numcols_;
  }
  [[nodiscard]] constexpr auto is_optimal() const noexcept -> bool
  {
    return m_is_optimal_;
  }
  [[nodiscard]] constexpr auto backend_solution_fresh() const noexcept -> bool
  {
    return m_backend_solution_fresh_;
  }
  [[nodiscard]] auto col_sol() const noexcept -> std::span<const double>
  {
    return m_col_sol_;
  }
  [[nodiscard]] auto col_cost() const noexcept -> std::span<const double>
  {
    return m_col_cost_;
  }
  [[nodiscard]] auto row_dual() const noexcept -> std::span<const double>
  {
    return m_row_dual_;
  }

  // ── Scalar write helpers ────────────────────────────────────────────────

  constexpr void set_obj_value(double v) noexcept { m_obj_value_ = v; }
  constexpr void set_kappa(std::optional<double> v) noexcept { m_kappa_ = v; }
  constexpr void set_numrows(Index v) noexcept { m_numrows_ = v; }
  constexpr void set_numcols(Index v) noexcept { m_numcols_ = v; }

  /// **C4** — flip the optimality flag.  The only path that may set
  /// it to `true`.  Callers that pass `false` here are equivalent to
  /// `invalidate_optimal_on_mutation()` (kept distinct for call-site
  /// readability).
  constexpr void set_is_optimal(bool v) noexcept { m_is_optimal_ = v; }

  /// **C5** — track whether the live backend's `col_solution()` /
  /// `is_proven_optimal()` reflect a fresh solve.  Cleared on
  /// `reconstruct_backend()` / `install_flat_as_rebuild()`; set on
  /// successful `resolve()` / `initial_solve()`.
  constexpr void mark_solution_fresh(bool v) noexcept
  {
    m_backend_solution_fresh_ = v;
  }

  // ── Span-out write buffers (C7) ─────────────────────────────────────────

  /// Resize the col_sol vector to `n` elements and return a writable
  /// span over it.  The single write entry-point used by
  /// `SolverBackend::fill_col_sol(span)` in compress mode.
  [[nodiscard]] auto col_sol_buffer(std::size_t n) -> std::span<double>
  {
    m_col_sol_.resize(n);
    return {m_col_sol_.data(), m_col_sol_.size()};
  }
  [[nodiscard]] auto col_cost_buffer(std::size_t n) -> std::span<double>
  {
    m_col_cost_.resize(n);
    return {m_col_cost_.data(), m_col_cost_.size()};
  }
  [[nodiscard]] auto row_dual_buffer(std::size_t n) -> std::span<double>
  {
    m_row_dual_.resize(n);
    return {m_row_dual_.data(), m_row_dual_.size()};
  }

  // ── Higher-level mutation hooks ─────────────────────────────────────────

  /// **C2** — flip `is_optimal` to false.  Idempotent.  Does NOT
  /// clear the col_sol / col_cost / row_dual vectors (some readers
  /// in compress mode still want the last-known values via the
  /// empty-fallback path even after a structural mutation).
  constexpr void invalidate_optimal_on_mutation() noexcept
  {
    m_is_optimal_ = false;
  }

  /// **C3** — drop col_sol + col_cost (the largest-footprint
  /// vectors), keep row_dual + scalars + optimality flag.  Used
  /// after the iteration's state CSV has been written and the
  /// backward pass has consumed any reduced-cost data — the row
  /// duals + objective value are still needed downstream for output
  /// writing.
  void drop_solution_caches() noexcept
  {
    m_col_sol_.clear();
    m_col_sol_.shrink_to_fit();
    m_col_cost_.clear();
    m_col_cost_.shrink_to_fit();
  }

  /// Drop ALL three vectors and clear the optimality flag.  Used by
  /// `reconstruct_backend()` and `release_backend()` paths that
  /// fully invalidate the cached solve.
  void clear_all_solution_vectors() noexcept
  {
    m_col_sol_.clear();
    m_col_sol_.shrink_to_fit();
    m_col_cost_.clear();
    m_col_cost_.shrink_to_fit();
    m_row_dual_.clear();
    m_row_dual_.shrink_to_fit();
  }

  // ── Diagnostic ──────────────────────────────────────────────────────────

  /// **C6** — total bytes held by the three solution vectors.
  /// Used by `LinearInterface::cache_size_bytes()` for memory-usage
  /// reporting / TUI dashboards.
  [[nodiscard]] auto size_bytes() const noexcept -> std::size_t
  {
    return (m_col_sol_.size() + m_col_cost_.size() + m_row_dual_.size())
        * sizeof(double);
  }

  /// **C1** debug self-check.  Returns true iff the cache is
  /// internally consistent (`is_optimal` ⇒ vector sizes match
  /// scalar counts).  Returns true when `is_optimal` is false (no
  /// constraint on vector sizes in that case).
  [[nodiscard]] auto validate_consistency() const noexcept -> bool
  {
    if (!m_is_optimal_) {
      return true;
    }
    const auto ncols_sz = static_cast<std::size_t>(m_numcols_);
    const auto nrows_sz = static_cast<std::size_t>(m_numrows_);
    // col_sol / col_cost are populated together; row_dual is
    // populated independently (compress mode only writes all 3
    // simultaneously, but `drop_solution_caches()` clears 2/3 while
    // leaving `is_optimal` set).  Treat empty vectors as "valid
    // (already consumed)" rather than a violation.
    const bool col_sol_ok = m_col_sol_.empty() || m_col_sol_.size() == ncols_sz;
    const bool col_cost_ok =
        m_col_cost_.empty() || m_col_cost_.size() == ncols_sz;
    const bool row_dual_ok =
        m_row_dual_.empty() || m_row_dual_.size() == nrows_sz;
    return col_sol_ok && col_cost_ok && row_dual_ok;
  }

private:
  double m_obj_value_ {};
  std::optional<double> m_kappa_ {};
  Index m_numrows_ {};
  Index m_numcols_ {};
  bool m_is_optimal_ {false};
  /// True iff the LIVE backend's `col_solution()` /
  /// `is_proven_optimal()` reflect a fresh solve.  See
  /// `mark_solution_fresh(...)` for the lifecycle contract.
  bool m_backend_solution_fresh_ {false};
  std::vector<double> m_col_sol_ {};
  std::vector<double> m_col_cost_ {};
  std::vector<double> m_row_dual_ {};
};

}  // namespace gtopt
