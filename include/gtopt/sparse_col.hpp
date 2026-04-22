/**
 * @file      sparse_col.hpp
 * @brief     Sparse column representation for linear programming variables
 * @date      Thu May 15 19:15:07 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides the SparseCol class which represents variables in a
 * linear program, including their bounds, objective coefficients, and
 * integrality constraints.
 */

#pragma once

#include <limits>
#include <ranges>

#include <gtopt/basic_types.hpp>
#include <gtopt/lp_context.hpp>

namespace gtopt
{

/// Maximum representable double value used for unbounded variable bounds.
constexpr double DblMax = std::numeric_limits<double>::max();

/**
 * @class SparseCol
 * @brief Represents a variable column in a linear program
 *
 * This class provides a fluent interface for setting variable properties
 * including bounds, objective coefficients, integrality constraints, and
 * a physical-to-LP scale factor.
 *
 * The @c scale field records the relationship between the LP variable and
 * the corresponding physical quantity:
 *
 *   physical_value = LP_value × scale
 *
 * For example, a voltage angle column with `scale_theta = 0.0001` stores
 * `scale = 0.0001` because `theta_physical = theta_LP × 0.0001`.
 * A reservoir energy column with `energy_scale = 100000` stores
 * `scale = 100000` because `volume_physical = volume_LP × 100000`.
 * All scales follow the same convention — no inversions.
 *
 * The scale is used by:
 * - Output rescaling: primal `x_phys = x_LP × scale`, reduced cost
 *   `rc_phys = rc_LP / scale`.
 * - User constraints (PAMPL): `LinearProblem::get_col_scale()` returns the
 *   scale so that constraint coefficients are correctly adjusted.
 * - Cross-element references (e.g. ReservoirSeepageLP reading reservoir volume
 *   scale to convert seepage slope to LP units).
 *
 * All methods are constexpr to enable compile-time construction of problems.
 */
struct SparseCol
{
  double lowb {0.0};  ///< Physical lower bound (default: 0.0)
  double uppb {DblMax};  ///< Physical upper bound (default: +infinity)
  double cost {0.0};  ///< Objective coefficient (default: 0.0)
  bool is_integer {false};  ///< is integer-constrained (default: false)
  bool is_state {false};  ///< True when the column is a state variable used
                          ///< in cascade/SDDP cut I/O.  Column names are
                          ///< available when `LpNamesLevel::all` is set;
                          ///< state variable I/O uses the StateVariable map
                          ///< (ColIndex-based) directly.  Set via
                          ///< `SystemContext::add_state_col()`.
  double scale {1.0};  ///< Physical-to-LP scale: physical_value = LP_value ×
                       ///< scale (default: 1.0 = no scaling)

  /// Optional metadata for automatic VariableScaleMap resolution.
  /// When @c class_name is non-empty and @c scale is still 1.0,
  /// LinearProblem::add_col() looks up the scale from its VariableScaleMap.
  /// If @c class_name is empty (default), no lookup is performed.
  std::string_view class_name {};  ///< Element class (e.g. "Bus", "Reservoir")
  std::string_view
      variable_name {};  ///< Variable name (e.g. "theta", "energy")
  Uid variable_uid {unknown_uid};  ///< Element UID for per-element lookup
  LpContext context {};  ///< LP hierarchy context (scenario, stage, block, ...)

  /**
   * Sets variable to a fixed value (equality constraint)
   * @param value Fixed value to set
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& equal(double value) noexcept
  {
    lowb = value;
    uppb = value;
    return *this;
  }

  /**
   * Sets variable to be free (unbounded in both directions)
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& free() noexcept
  {
    lowb = -DblMax;
    uppb = DblMax;
    return *this;
  }

  /**
   * Marks variable as integer-constrained
   * @return Reference to this column for method chaining
   */
  constexpr SparseCol& integer() noexcept
  {
    is_integer = true;
    return *this;
  }
};

/// Lightweight label metadata for a column.
///
/// Stored in `FlatLinearProblem` / `LinearInterface` so LP column
/// names can be formatted on demand at `write_lp` time instead of
/// eagerly at flatten.  This is the only column data that
/// `LabelMaker` needs; bounds / cost / scale live in the flat LP.
///
/// Carries the same `(class_name, variable_name, variable_uid,
/// context)` 4-tuple that `SparseCol` uses — a `LocalLabelMaker` at
/// `LpNamesLevel::all` can synthesise the human-readable label
/// (e.g. `bus.theta.1.s0.p0.b0`) from this struct alone.
struct SparseColLabel
{
  std::string_view class_name {};
  std::string_view variable_name {};
  Uid variable_uid {unknown_uid};
  LpContext context {};

  friend constexpr bool operator==(const SparseColLabel&,
                                   const SparseColLabel&) noexcept = default;
};

/// Hash functor for `SparseColLabel`, used by the eager duplicate-detection
/// map in `LinearInterface`.  Combines each field via `detail::hash_combine`
/// and dispatches on the `LpContext` variant through `std::visit`.
struct SparseColLabelHash
{
  [[nodiscard]] size_t operator()(const SparseColLabel& l) const noexcept
  {
    size_t h = std::hash<std::string_view> {}(l.class_name);
    h = detail::hash_combine(h,
                             std::hash<std::string_view> {}(l.variable_name));
    h = detail::hash_combine(h, std::hash<Uid> {}(l.variable_uid));
    h = detail::hash_combine(
        h,
        std::visit(
            []<typename T>(const T& t) noexcept -> size_t
            {
              if constexpr (std::is_same_v<T, std::monostate>) {
                return 0;
              } else {
                return TupleHash {}(t);
              }
            },
            l.context));
    return h;
  }
};

using ColIndex = StrongIndexType<SparseCol>;  ///< Type alias for column index

/**
 * @brief Build a `ColIndex` from the size of any sized range.
 *
 * Centralises the `ColIndex{static_cast<Index>(r.size())}` pattern so
 * that callers stay in strong-index space.  Typical call sites:
 *
 * @code
 *   if (col < col_index_size(col_sol)) { ... }
 *   for (auto idx : iota_range<ColIndex>(ColIndex{0}, col_index_size(cols)))
 * @endcode
 *
 * @tparam R  Any `std::ranges::sized_range`.
 * @param  r  The range whose size should be interpreted as a column count.
 * @return    `ColIndex{static_cast<Index>(std::ranges::size(r))}`.
 */
template<std::ranges::sized_range R>
[[nodiscard]] constexpr auto col_index_size(const R& r) noexcept -> ColIndex
{
  return ColIndex {static_cast<Index>(std::ranges::size(r))};
}

// ── Scale-aware output helpers ───────────────────────────────────────────────

/// Returns a lambda that rescales LP primal values to physical units.
/// physical_value = LP_value × scale.
///
/// Usage in add_to_output:
///   out.add_col_sol(cname, "theta", pid, theta_cols, col_scale_sol(scale));
[[nodiscard]] constexpr auto col_scale_sol(double scale) noexcept
{
  return [scale](double lp_value) noexcept { return lp_value * scale; };
}

/// Returns a lambda that rescales LP reduced costs to physical units.
/// rc_physical = rc_LP / scale  (inverse of primal rescaling).
///
/// Usage in add_to_output:
///   out.add_col_cost(cname, "theta", pid, theta_cols, col_scale_cost(scale));
[[nodiscard]] constexpr auto col_scale_cost(double scale) noexcept
{
  return [inv = 1.0 / scale](double lp_cost) noexcept { return lp_cost * inv; };
}

}  // namespace gtopt
