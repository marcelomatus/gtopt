/**
 * @file      lp_scaling.hpp
 * @brief     LP-scaling subsystem of an LP matrix
 * @date      2026-07-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Groups the Ruiz / equilibration scaling state of a `LinearInterface`
 * into a behaviour-free value aggregate, plus the zero-copy `ScaledView`
 * helper that applies a per-element scale factor.  Extracted from
 * `LinearInterface` as step 4 (final) of decomposing that ~3170-line
 * class (mirrors the `MatrixStats` / `LpLabelStore` precedents).
 *
 * Behaviour is preserved verbatim — this is a pure data grouping:
 *   * The COW-shared scale vectors / cost-scale-type vectors and the
 *     variable-scale map are `shared_ptr`-wrapped and `mutable` so
 *     shallow clones share state via atomic incref and const paths can
 *     lazily detach-for-write.  Every `make_shared<>` default is kept.
 *   * `equilibration_method` / `obj_constant_raw` are plain per-instance
 *     values (never `shared_ptr`, never `mutable`), value-copied on a
 *     clone — matching the pre-extraction member-by-member behaviour.
 */

#pragma once

#include <algorithm>  // std::clamp
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gtopt/basic_types.hpp>  // Index
#include <gtopt/lp_matrix_enums.hpp>  // LpEquilibrationMethod
#include <gtopt/sparse_col.hpp>  // ColIndex
#include <gtopt/sparse_row.hpp>  // RowIndex
#include <gtopt/strong_index_vector.hpp>  // StrongIndexVector
#include <gtopt/user_constraint_enums.hpp>  // ConstraintScaleType
#include <gtopt/variable_scale.hpp>  // VariableScaleMap

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

/// LP-scaling subsystem of a `LinearInterface`.  Groups the Ruiz /
/// equilibration column-row scale vectors, the per-column / per-row
/// objective time-basis (cost-scale-type) vectors, the variable-scale
/// map, the equilibration method, and the raw objective constant.
/// Extracted verbatim from `LinearInterface` — every `mutable`
/// qualifier and default initializer is preserved so the COW / clone
/// semantics are bit-identical to the pre-extraction code.
struct ScalingState
{
  /// Constant offset added to `get_obj_value_raw()`.  Stored on the
  /// LP *raw* (post-scale_objective-division) cost scale so it
  /// composes additively with the solver's raw value:
  ///
  ///   get_obj_value_raw() = solver_raw + obj_constant_raw
  ///   get_obj_value()     = get_obj_value_raw() × m_scale_objective_
  ///
  /// Propagated from `FlatLinearProblem::obj_constant_raw` at
  /// `load_flat` time.  Copied through native clone / clone-from-flat
  /// so clones report the same algebraic objective.  Default 0.0 —
  /// every model that does not opt in keeps bit-identical raw / phys
  /// reports.
  ///
  /// Public mutators `add_obj_constant` / `obj_constant` are declared
  /// in the class's public section near `get_obj_value()` so callers
  /// can adjust the constant POST-`load_flat`.  Callers always pass
  /// values in *physical* units; the API divides by
  /// `m_scale_objective_` before accumulating here.
  double obj_constant_raw {0.0};

  /// Column / row scale vectors.  `shared_ptr` so shallow clones
  /// can share with the source — see `CloneKind`.  The scale vectors
  /// are populated in `load_flat` and only mutated post-flatten by
  /// `set_col_scale` / `set_row_scale` (called when a non-unit
  /// `col.scale` / `row.scale` is added via `add_col(SparseCol)` /
  /// `add_row_raw`).  Disposable adds explicitly forbid non-unit
  /// scales (see `add_col_disposable` / `add_row_disposable`) so
  /// they never trigger the COW detach branch on the clone side.
  mutable std::shared_ptr<StrongIndexVector<ColIndex, double>> col_scales {
      std::make_shared<StrongIndexVector<ColIndex, double>>(),
  };
  mutable std::shared_ptr<StrongIndexVector<RowIndex, double>> row_scales {
      std::make_shared<StrongIndexVector<RowIndex, double>>(),
  };
  /// Per-column / per-row objective time-basis (Power / Energy / Raw),
  /// populated from `FlatLinearProblem::col_cost_scale_types` /
  /// `row_cost_scale_types` at `load_flat`.  Frozen after flatten (never
  /// mutated post-load), so `shared_ptr` lets shallow clones share without
  /// copying — same lifecycle as `col_scales`.  Consumed by
  /// `OutputContext` to choose the inverse cost-factor family when reading
  /// reduced costs / duals back to physical units.  Out-of-range (post-
  /// flatten) indices default to `Power` via `*_cost_scale_type_at`.
  mutable std::shared_ptr<std::vector<ConstraintScaleType>>
      col_cost_scale_types {
          std::make_shared<std::vector<ConstraintScaleType>>(),
      };
  mutable std::shared_ptr<std::vector<ConstraintScaleType>>
      row_cost_scale_types {
          std::make_shared<std::vector<ConstraintScaleType>>(),
      };
  /// Equilibration method used at load_flat() time.  Persisted so that
  /// `add_row` / `add_rows` (the post-build cut path) apply the same
  /// per-row scaling the bulk build did, keeping kappa stable as cuts
  /// accumulate.  `none` means the caller opted out of equilibration
  /// at build time and we leave new rows alone.
  LpEquilibrationMethod equilibration_method {LpEquilibrationMethod::none};
  /// Moved from flatten.  `shared_ptr` so shallow clones can share
  /// it with the source instead of value-copying — see `CloneKind`.
  /// Mutated only via `load_flat` (source-side, before any clones)
  /// so the COW detach in `detach_for_write` is dormant in practice.
  mutable std::shared_ptr<VariableScaleMap> variable_scale_map {
      std::make_shared<VariableScaleMap>(),
  };
};

}  // namespace gtopt
