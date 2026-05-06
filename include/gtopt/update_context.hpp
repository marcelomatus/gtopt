/**
 * @file      update_context.hpp
 * @brief     Self-cached context for HasUpdateLP elements
 * @date      2026-05-05
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Each LP element that satisfies the `HasUpdateLP` concept (`update_lp()`
 * mutates LP coefficients per SDDP iteration) reads a small amount of
 * reservoir state — eini volume, efin volume — at update time.  Routing
 * those reads through `sys.element<ReservoirLP>(...)` couples
 * `update_lp` to the current SystemLP's `Collection<ReservoirLP>`,
 * blocking the Phase-2 collection-drop optimisation under
 * `LowMemoryMode::compress`.
 *
 * `ReservoirRefCache` captures, at `add_to_lp` time, exactly what
 * `update_lp` needs:
 *   * `default_volume` — fallback value for un-solved phases.
 *   * `energy_scale`  — scale factor for warm-solution reads.
 *   * `eini_col` / `efin_col` — current-stage column indices.
 *   * `rsv_uid` — kept for diagnostics and identity.
 *
 * `physical_eini_from_cache` and `physical_efin_from_cache` read the
 * cached column indices directly from `sys.linear_interface()`.  They
 * never call `sys.element<ReservoirLP>` and never traverse
 * `prev_phase_sys`.  This is sufficient because:
 *
 *   * **Non-daily_cycle reservoir** — the LP's state-link constraint
 *     pins `eini_col == prev_phase_efin_col`, so reading current
 *     `eini_col` yields the same value as the predecessor's solved
 *     efin, with no cross-phase traversal.
 *
 *   * **Daily_cycle reservoir** — there's no state link; the in-phase
 *     `efin == eini` constraint pins both columns together; reading
 *     current `eini_col` yields the current phase's optimised volume.
 *
 * In both cases, "read the eini column" is the right answer and there
 * is no special-case for daily_cycle.
 */

#pragma once

#include <cstddef>

#include <gtopt/basic_types.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

struct ReservoirRefCache
{
  Uid rsv_uid {};
  Real default_volume {0.0};
  Real energy_scale {1.0};
  ColIndex eini_col {};
  ColIndex efin_col {};
};

/// Per-(scenario, stage) cached state for elements whose `update_lp`
/// evaluates a `RightBoundRule` axis to refresh a column bound.
///
/// Shared by `FlowRightLP` and `VolumeRightLP` — both elements drive
/// `update_lp` from a single `right_bound_rule` and need exactly:
///   * `current_bound`    — last evaluated rule output (axis -> bound)
///                          to skip redundant `set_col_lowb/uppb` calls.
///   * `reservoir_cache`  — cached column indices for reservoir-axis
///                          rules; default-constructed for axes that
///                          do not consume reservoir state.
///
/// The struct is intentionally aggregate-trivial so that
/// `IndexHolder2<ScenarioUid, StageUid, RuleBoundState>` benefits from
/// trivial copy/move during collection rebuilds.
struct RuleBoundState
{
  Real current_bound {0.0};
  ReservoirRefCache reservoir_cache {};
};

/// Populate a `ReservoirRefCache` from a `ReservoirLP` reference.
[[nodiscard]] inline ReservoirRefCache make_reservoir_ref_cache(
    const ReservoirLP& rsv, const ScenarioLP& scenario, const StageLP& stage)
{
  return ReservoirRefCache {
      .rsv_uid = rsv.uid(),
      .default_volume = rsv.reservoir().eini.value_or(0.0),
      .energy_scale = rsv.energy_scale(),
      .eini_col = rsv.eini_col_at(scenario, stage),
      .efin_col = rsv.efin_col_at(scenario, stage),
  };
}

/// Read the physical efin of the current stage from
/// `sys.linear_interface()` using the cached `efin_col`.
template<typename SystemLPT>
[[nodiscard]] Real physical_efin_from_cache(
    const SystemLPT& sys, const ReservoirRefCache& rc) noexcept
{
  const auto& li = sys.linear_interface();
  if (li.is_optimal()) {
    return li.get_col_sol()[rc.efin_col];
  }
  const auto& warm = li.warm_col_sol();
  if (!warm.empty() && static_cast<std::size_t>(rc.efin_col) < warm.size()) {
    return warm[rc.efin_col] * rc.energy_scale;
  }
  return rc.default_volume;
}

/// Read the physical eini of the current stage from
/// `sys.linear_interface()` using the cached `eini_col`.
///
/// This is intentionally symmetric with `physical_efin_from_cache`:
/// for any storage element (reservoir, battery, volume_right) the
/// LP's structural constraints (state-link or daily-cycle close)
/// guarantee that `li.get_col_sol()[eini_col]` is the right value
/// for segment selection in `update_lp`, in both daily_cycle and
/// non-daily_cycle modes.  See file docstring.
template<typename SystemLPT>
[[nodiscard]] Real physical_eini_from_cache(
    [[maybe_unused]] const SystemLPT& sys,
    [[maybe_unused]] const ScenarioLP& scenario,
    const StageLP& stage,
    const ReservoirRefCache& rc) noexcept
{
  if (!stage.index() && !stage.phase_index()) {
    return rc.default_volume;
  }
  const auto& li = sys.linear_interface();
  if (li.is_optimal()) {
    return li.get_col_sol()[rc.eini_col];
  }
  const auto& warm = li.warm_col_sol();
  if (!warm.empty() && static_cast<std::size_t>(rc.eini_col) < warm.size()) {
    return warm[rc.eini_col] * rc.energy_scale;
  }
  return rc.default_volume;
}

/// Convenience: return the (vini + vfin) / 2 average that all four
/// reservoir-driven `update_lp` impls (FlowRight, RDL, ProductionFactor,
/// VolumeRight) use as the bound-rule axis input.  ReservoirSeepageLP
/// uses `physical_eini_from_cache` directly (vini-only, by design).
template<typename SystemLPT>
[[nodiscard]] Real average_volume_from_cache(const SystemLPT& sys,
                                             const ScenarioLP& scenario,
                                             const StageLP& stage,
                                             const ReservoirRefCache& rc)
{
  const auto vini = physical_eini_from_cache(sys, scenario, stage, rc);
  const auto vfin = physical_efin_from_cache(sys, rc);
  return (vini + vfin) / 2.0;
}

}  // namespace gtopt
