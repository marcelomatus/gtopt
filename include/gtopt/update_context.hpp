/**
 * @file      update_context.hpp
 * @brief     Self-cached context for HasUpdateLP elements
 * @date      2026-05-05
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Each LP element that satisfies the `HasUpdateLP` concept (`update_lp()`
 * mutates LP coefficients per SDDP iteration) reads a small amount of
 * state from a peer element — currently only `ReservoirLP` — at update
 * time.  Routing those reads through `sys.element<ReservoirLP>(...)`
 * couples `update_lp` to the current SystemLP's `Collection<ReservoirLP>`,
 * which blocks the Phase-2 collection-drop optimisation (compress mode).
 *
 * Cross-phase reservoir state propagates through gtopt's existing
 * `StateVariable` / `StateVariableLink` channel, not through the
 * predecessor LP's column solution: every reservoir's `efin` column is
 * registered with `SimulationLP::add_state_variable` at `add_to_lp`
 * time, and the SDDP forward pass writes the post-solve value via
 * `capture_state_variable_values`.  `update_lp` therefore only needs a
 * stable pointer to that `StateVariable` to read the predecessor's
 * efin — no `prev_sys->element<X>(...)`, no `prev_li.get_col_sol()[col]`.
 *
 * `ReservoirRefCache` captures, at `add_to_lp` time:
 *   * `default_volume` — the static input fallback
 *   * `eini_col` / `efin_col` — current-stage column indices
 *   * `energy_scale` — required to physical-rescale `warm_col_sol` reads
 *   * `rsv_sid` — single-id of the source reservoir; used by
 *      `bind_prev_phase_state_var` during the post-build link pass and
 *      as a defensive fallback when running outside PlanningLP (i.e.
 *      tests that do not invoke `tighten_scene_phase_links`).
 *
 * Plus, bound by `bind_prev_phase_state_var` during the post-build
 * tightening pass:
 *   * `has_prev_phase` + `prev_phase_index` + `prev_phase_last_stage_uid`
 *      — locator for the predecessor phase's efin `StateVariable`.
 *      `physical_eini_from_cache` builds a `StateVariable::Key` from
 *      the locator and looks it up in the simulation registry on
 *      every read.  We deliberately do not cache a `const StateVariable*`
 *      — `flat_map` is vector-backed and `LowMemoryMode::rebuild` may
 *      reallocate it via `add_state_variable`, silently dangling the
 *      pointer.
 *
 * The free helpers in this header reproduce `StorageLP::physical_eini`
 * and `StorageLP::physical_efin` byte-for-byte against the cache, so
 * callers see identical numerics whether they query via the cache or
 * via `sys.element<ReservoirLP>(...).physical_*`.
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

/// Static reservoir context captured at `add_to_lp` time, plus a
/// predecessor-phase locator (`has_prev_phase`, `prev_phase_index`,
/// `prev_phase_last_stage_uid`) populated by `tighten_scene_phase_links`
/// once every phase in a scene is built.
///
/// Population happens in two phases:
///   * `make_reservoir_ref_cache` — fills current-phase fields at
///     `add_to_lp` time (when the source reservoir is guaranteed alive
///     in the same SystemLP).
///   * `bind_prev_phase_state_var` — sets the predecessor locator once
///     the full phase chain has been registered.  Runs from
///     `tighten_scene_phase_links` so it is build-mode-agnostic (works
///     under serial, scene_parallel, full_parallel, direct_parallel).
///
/// `rsv_sid` is retained as a fallback key for unit tests that
/// construct a SystemLP without going through PlanningLP.  In that
/// path `has_prev_phase` stays false and `physical_eini_from_cache`
/// falls back to `prev_sys->element<ReservoirLP>(rsv_sid).efin_col_at(...)`.
struct ReservoirRefCache
{
  ReservoirLPSId rsv_sid;
  Uid rsv_uid {};
  Real default_volume {0.0};
  Real energy_scale {1.0};
  ColIndex eini_col {};
  ColIndex efin_col {};
  /// Locator for the predecessor's efin `StateVariable`.  Set once per
  /// (sys, prev_sys) pair by `bind_prev_phase_state_var`; consumed at
  /// every `physical_eini_from_cache` call as a key into the
  /// `SimulationLP` state-variable registry.
  ///
  /// We deliberately do NOT cache a `const StateVariable*` here even
  /// though it would save one `flat_map::find` per `update_lp` call.
  /// The simulation's per-(scene, phase) state-variable map is a
  /// vector-backed `flat_map`; under `LowMemoryMode::rebuild`,
  /// `rebuild_in_place()` re-runs `create_lp()` and may register new
  /// state variables, which would reallocate the underlying vector
  /// and silently invalidate any cached interior pointer.  A cached
  /// pointer would then dangle and feed `update_lp` a wrong (or worse,
  /// undefined-behaviour-derived) efin value — exactly the
  /// hard-to-debug regression class we want to avoid.  Looking up the
  /// StateVariable by key on every read costs ~O(log N) on a small
  /// per-(scene, phase) map and is provably safe across all
  /// `LowMemoryMode` settings.
  bool has_prev_phase {false};
  PhaseIndex prev_phase_index {};
  StageUid prev_phase_last_stage_uid {};
};

/// Populate a `ReservoirRefCache` from a `ReservoirLP` reference.
///
/// Resolves the same data `update_lp` would read at iteration time, but
/// at `add_to_lp` time when the source reservoir is guaranteed alive.
[[nodiscard]] inline ReservoirRefCache make_reservoir_ref_cache(
    const ReservoirLP& rsv,
    const ReservoirLPSId& rsv_sid,
    const ScenarioLP& scenario,
    const StageLP& stage)
{
  return ReservoirRefCache {
      .rsv_sid = rsv_sid,
      .rsv_uid = rsv.uid(),
      .default_volume = rsv.reservoir().eini.value_or(0.0),
      .energy_scale = rsv.energy_scale(),
      .eini_col = rsv.eini_col_at(scenario, stage),
      .efin_col = rsv.efin_col_at(scenario, stage),
  };
}

/// One-shot binder for the predecessor-phase efin locator.  Records
/// `(prev_phase_index, prev_phase_last_stage_uid)` so
/// `physical_eini_from_cache` can build a `StateVariable::Key` and
/// look up the predecessor's StateVariable on every call.  We do NOT
/// look up the StateVariable here and cache a pointer — see the
/// `has_prev_phase` rationale on `ReservoirRefCache` for why the
/// pointer-cache form is unsafe under `LowMemoryMode::rebuild`.
///
/// Invoked from `tighten_scene_phase_links` after every phase has
/// finished `add_to_lp`.  Idempotent: re-binding against the same
/// `prev_sys` yields the same locator.  The two extra parameters
/// (`scene_index`, `scenario_uid`) are accepted for API symmetry with
/// the convenience overload below; the locator alone is sufficient
/// because `scenario_uid` is part of the per-(scen, stg) cache map
/// key already, and `scene_index` is shared with the current sys.
template<typename SimulationLPT>
inline void bind_prev_phase_state_var(
    ReservoirRefCache& rc,
    [[maybe_unused]] const SimulationLPT& simulation,
    [[maybe_unused]] SceneIndex scene_index,
    PhaseIndex prev_phase_index,
    [[maybe_unused]] ScenarioUid scenario_uid,
    StageUid prev_last_stage_uid) noexcept
{
  rc.has_prev_phase = true;
  rc.prev_phase_index = prev_phase_index;
  rc.prev_phase_last_stage_uid = prev_last_stage_uid;
}

/// Convenience overload accepting `ScenarioLP` / `StageLP` references —
/// used by the unit tests so they don't have to construct primitives.
template<typename SimulationLPT>
inline void bind_prev_phase_state_var(ReservoirRefCache& rc,
                                      const SimulationLPT& simulation,
                                      const ScenarioLP& scenario,
                                      const StageLP& prev_last_stage) noexcept
{
  bind_prev_phase_state_var(rc,
                            simulation,
                            scenario.scene_index(),
                            prev_last_stage.phase_index(),
                            scenario.uid(),
                            prev_last_stage.uid());
}

/// Read the physical efin of the current stage from `sys.linear_interface()`,
/// using the cached `efin_col`.  Replicates `StorageLP::physical_efin`
/// (li-only overload) without reaching back into the current
/// SystemLP's `Collection<ReservoirLP>`.
template<typename SystemLPT>
[[nodiscard]] Real physical_efin_from_cache(
    const SystemLPT& sys, const ReservoirRefCache& rc) noexcept
{
  const auto& li = sys.linear_interface();
  if (li.is_optimal()) {
    // Physical, optimal-only bound-clamped: matches storage_lp.hpp:404-415.
    return li.get_col_sol()[rc.efin_col];
  }
  const auto& warm = li.warm_col_sol();
  if (!warm.empty() && static_cast<std::size_t>(rc.efin_col) < warm.size()) {
    // Warm sol stored in scaled units — undo with energy_scale.
    return warm[rc.efin_col] * rc.energy_scale;
  }
  return rc.default_volume;
}

/// Read the physical eini of the current stage, replicating the
/// cross-phase fallback chain in `StorageLP::physical_eini`
/// (sys-overload, storage_lp.hpp:304-372).
///
/// Cross-phase resolution uses `rc.{has_prev_phase, prev_phase_index,
/// prev_phase_last_stage_uid}` to build a `StateVariable::Key` and
/// look it up in the SimulationLP-wide state-variable registry.
/// Reading `state_var.col_sol_physical()` consumes the exact value
/// `capture_state_variable_values` wrote after the predecessor's
/// forward solve — algebraically identical to
/// `prev_li.get_col_sol()[efin_col]` but without touching the
/// predecessor's LP backend, element collection, or column-solution
/// vector.
///
/// `prev_sys->linear_interface().is_optimal()` is still the gate: it
/// is a cached flag (no LP backend access required) and tells whether
/// the predecessor has actually been solved yet — col_sol stays at
/// 0.0 before the first forward solve and would otherwise be returned
/// blindly.
///
/// Falls back to the element-lookup path when the locator is unbound —
/// typical for unit tests that build a SystemLP without going through
/// PlanningLP / `tighten_scene_phase_links`.  The fallback yields
/// byte-identical numerics to the lookup path.
///
/// **Why per-call lookup instead of cached pointer**: `flat_map` is
/// vector-backed; under `LowMemoryMode::rebuild` the simulation may
/// re-register state variables and reallocate the underlying vector,
/// which would silently invalidate any cached pointer.  Looking up
/// the key on every read is provably safe across all `LowMemoryMode`
/// settings.
template<typename SystemLPT>
[[nodiscard]] Real physical_eini_from_cache(const SystemLPT& sys,
                                            const ScenarioLP& scenario,
                                            const StageLP& stage,
                                            const ReservoirRefCache& rc)
{
  if (!stage.index() && !stage.phase_index()) {
    return rc.default_volume;
  }

  // 1. Cross-phase: predecessor's optimal efin (storage_lp.hpp:351-362).
  if (const auto* prev_sys = sys.prev_phase_sys()) {
    const auto& prev_li = prev_sys->linear_interface();
    if (prev_li.is_optimal()) {
      // Fast path — locate the predecessor's efin StateVariable by
      // key.  Equivalent to (and safer than) caching a `const
      // StateVariable*`; see `ReservoirRefCache::has_prev_phase`
      // docstring.
      if (rc.has_prev_phase) {
        static constexpr auto reservoir_class_name =
            ReservoirLP::Element::class_name;
        auto state_var = sys.system_context().simulation().state_variable(
            StateVariable::key(reservoir_class_name,
                               rc.rsv_uid,
                               StorageLP<ObjectLP<Reservoir>>::EfinName,
                               rc.prev_phase_index,
                               rc.prev_phase_last_stage_uid,
                               scenario.scene_index(),
                               scenario.uid()));
        if (state_var.has_value()) {
          return state_var->get().col_sol_physical();
        }
      }
      // Fallback for paths that bypass `tighten_scene_phase_links`
      // (unit tests) or for daily-cycle reservoirs whose efin
      // StateVariable is intentionally not registered.  Goes through
      // the element lookup but yields identical numerics.
      const auto& prev_rsv =
          prev_sys->template element<ReservoirLP>(rc.rsv_sid);
      const auto& prev_stages = prev_sys->phase().stages();
      if (!prev_stages.empty()) {
        return prev_rsv.physical_efin(
            prev_li, scenario, prev_stages.back(), rc.default_volume);
      }
    }
  }

  // 2. Hot-start warm value (storage_lp.hpp:363-369).
  const auto& li = sys.linear_interface();
  const auto& warm = li.warm_col_sol();
  if (!warm.empty() && static_cast<std::size_t>(rc.eini_col) < warm.size()) {
    return warm[rc.eini_col] * rc.energy_scale;
  }

  // 3. Default fallback.
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
