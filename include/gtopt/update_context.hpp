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
#include <gtopt/state_variable.hpp>

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
  return rc.default_volume;
}

/// Read the physical eini of the current stage — the predecessor
/// phase's solved efin propagated via gtopt's existing
/// ``StateVariable`` channel.
///
/// Resolution order:
///
///   1. **Predecessor's efin StateVariable** (cross-phase).  When
///      ``sys.prev_phase_sys()`` is set (production path —
///      ``SDDPMethod::update_lp_for_phase`` wires it at
///      ``sddp_method_iteration.cpp:281``), look up the predecessor's
///      ``efin`` ``StateVariable`` by key in the simulation registry
///      and return ``col_sol_physical()``.  ``set_col_sol`` is called
///      on every ``StateVariable`` immediately after the predecessor's
///      forward solve, so by the time this phase's ``update_lp`` runs
///      the value is already correct — no LI query, no
///      ``is_optimal()`` check on the current phase's still-unsolved
///      LP.  Daily-cycle reservoirs have no state link and are not
///      reached here (their predecessor lookup returns no match).
///   2. **Current LI's eini_col solution** (in-phase).  Daily-cycle
///      reservoirs pin ``eini == efin`` via the close constraint, so
///      reading the current ``eini_col`` after the LP is solved is the
///      right answer.  Also covers test paths that don't set
///      ``prev_phase_sys``.
///   3. **Default volume**.  Iter-0 first phase, before any solve.
///
/// **Why the StateVariable channel** (and not the propagated col
/// bound, or ``prev_li.get_col_sol()`` directly):
///   * The current LI's ``eini_col`` solution is unavailable when
///     ``update_lp`` runs — that's BEFORE the solve.  The pre-existing
///     fallback to ``rc.default_volume`` (= JSON ``eini``) selected the
///     wrong piecewise segment for ReservoirSeepage/RDL/ProductionFactor
///     /FlowRight/VolumeRight whenever the predecessor's actual trial
///     differed from the JSON ``eini``.  On juan/IPLP scen 51 phase 38
///     ELTORO ``eini = 1731 hm³`` (segment 2 ``constant = 15.09 m³/s``)
///     vs predecessor's drained-to-0 trial → row forced 15.09 m³/s
///     seepage at empty storage → cascade infeasibility through
///     p38→p1.
///   * ``StateVariable::col_sol_physical()`` is gtopt's canonical
///     cross-phase value channel: it's updated post-solve and consumed
///     by ``propagate_trial_values`` and ``BendersCut`` exactly the
///     same way.  Reading it here makes the segment-selection axis
///     consistent with the rest of the SDDP machinery.
///   * Restores the design from commit ``7a446444 refactor(update_lp):
///     cache reservoir refs + use StateVariable channel`` that
///     ``aeeb1c98 simplify(update_context): read eini_col directly,
///     drop StateVariable detour`` removed under the (incorrect)
///     assumption that ``current_eini_col == prev_phase_efin_col``
///     algebraically when ``update_lp`` runs.
template<typename SystemLPT>
[[nodiscard]] Real physical_eini_from_cache(const SystemLPT& sys,
                                            const ScenarioLP& scenario,
                                            const StageLP& stage,
                                            const ReservoirRefCache& rc)
{
  if (!stage.index() && !stage.phase_index()) {
    return rc.default_volume;
  }

  // 1. Cross-phase via the StateVariable channel.
  if (const auto* prev_sys = sys.prev_phase_sys()) {
    const auto& prev_phase = prev_sys->phase();
    const auto& prev_stages = prev_phase.stages();
    if (!prev_stages.empty()) {
      const auto& prev_last = prev_stages.back();
      static constexpr auto reservoir_class_name =
          ReservoirLP::Element::class_name;
      // Predecessor's last-stage ``efin`` StateVariable.  The
      // ``"efin"`` literal matches ``StorageLP<>::EfinName``; using the
      // literal here avoids dragging in storage_lp.hpp from this
      // generic helper.
      auto svar_opt =
          sys.system_context().simulation().state_variable(StateVariable::key(
              scenario, prev_last, reservoir_class_name, rc.rsv_uid, "efin"));
      if (svar_opt.has_value()) {
        return svar_opt->get().col_sol_physical();
      }
    }
  }

  // 2. In-phase fallback (daily-cycle reservoirs, test paths without
  //    `prev_phase_sys`).
  const auto& li = sys.linear_interface();
  if (li.is_optimal()) {
    return li.get_col_sol()[rc.eini_col];
  }

  // 3. Default volume — iter-0 first phase before any solve.
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
