/**
 * @file      future_cost_lp.cpp
 * @brief     LP build / output path for FutureCost (FCF / cost-to-go) elements
 * @date      Sun Jun 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `add_to_global_lp` is still inert plumbing (the α-column registration,
 * boundary-cut load and mean_shift rebase live in the SDDP method).
 *
 * `add_to_output` SELF-FINDS its α columns + per-scene rebase constant at
 * write time from the persistent `SimulationLP` registries reached through the
 * `OutputContext`'s `SystemContext`.  Those registries survive the per-cell LP
 * rebuild that `write_out` performs under `low_memory = compress`, so the
 * FutureCost/{alpha|alpha_<s>, rebase} streams are emitted under ALL
 * `low_memory` modes — no per-cell resident stash is needed.
 */

#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/future_cost_lp.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

bool FutureCostLP::add_to_global_lp(const SystemContext& /*sc*/,
                                    const SceneLP& /*scene*/,
                                    const PhaseLP& /*phase*/,
                                    LinearProblem& /*lp*/)
{
  // Inert: α-column registration + boundary-cut load + mean_shift rebase
  // live in the SDDP method (register_alpha_variables + load_boundary_cuts).
  return true;
}

bool FutureCostLP::add_to_output(OutputContext& out) const
{
  static constexpr std::string_view cname = Element::class_name.full_name();

  // SELF-FIND: reach the persistent registries through the output context.
  // Both `sim` (state-variable map) and the scene/phase coordinates outlive
  // the per-cell LP rebuild that `write_out` performs under compress.
  const auto& sc = out.system_context();
  const auto& sim = sc.simulation();
  const auto& scene = sc.system().scene();
  const auto& phase = sc.system().phase();

  // α columns registered on this (scene, phase) cell, in source-scene order:
  // single layout → 1 entry; multicut → N varphi_s.  Empty when α was never
  // registered (e.g. last phase pinned, or no SDDP α at all) → nothing to do.
  std::vector<std::pair<ColIndex, Uid>> acols;
  if (future_cost().use_user_alpha.value_or(false)) {
    // User-overridable FCF (piece 5 step 2a): the built-in α is inert (not a
    // state var), so locate the user-authored α DecisionVariable (a `global`
    // `state`/`link` column under the dedicated `UserStateVar` class) by uid on
    // this cell and emit IT as `FutureCost/alpha`.  One column → single layout.
    if (const auto ua_uid = future_cost().user_alpha_uid; ua_uid.has_value()) {
      for (const auto& [key, svar] :
           sim.state_variables(scene.index(), phase.index()))
      {
        if (key.uid == *ua_uid
            && key.class_name == DecisionVariableLP::StateClassName)
        {
          acols.emplace_back(svar.col(), *ua_uid);
          break;
        }
      }
    }
  } else {
    acols = alpha_cols_on_cell(sim, scene.index(), phase.index());
  }
  if (acols.empty()) {
    return true;
  }

  const auto& stages = phase.stages();
  if (stages.empty()) {
    return true;
  }
  const auto& last_stage = stages.back();
  const auto& last_blocks = last_stage.blocks();
  if (last_blocks.empty()) {
    return true;
  }
  const auto last_block = last_blocks.back().uid();
  const bool multi = acols.size() > 1;

  // One α stream per registered column.  `add_col_sol` reads
  // `col_sol[α_col]` at the holder's terminal coords, so the α column's own
  // scene-phase context is irrelevant on the read side — we just place the
  // single terminal-block entry under every scenario of this scene.
  for (std::size_t s = 0; s < acols.size(); ++s) {
    const auto name =
        multi ? ("alpha_" + std::to_string(s)) : std::string(AlphaName);
    STBIndexHolder<ColIndex> cols;
    for (const auto& scenario : scene.scenarios()) {
      cols[std::tuple {scenario.uid(), last_stage.uid()}][last_block] =
          acols[s].first;
    }
    out.add_col_sol(cname, name, id(), cols);
  }

  // Per-scene α-rebase constant c̄ ($) — the mean_shift offset folded into the
  // objective; surfaced so `alpha + rebase` reconstructs the un-rebased FCF.
  const double c_bar = sim.alpha_offset(scene.index());
  if (c_bar != 0.0) {
    STBIndexHolder<double> rebase;
    for (const auto& scenario : scene.scenarios()) {
      rebase[std::tuple {scenario.uid(), last_stage.uid()}][last_block] = c_bar;
    }
    out.add_col_sol_values(cname, RebaseName, id(), rebase);
  }

  return true;
}

SDDPBoundaryConfig boundary_config(const FutureCost& fc)
{
  // The element fields are already `std::optional` of the same value types as
  // the config, so copy the optionals directly (an unset element field stays
  // `nullopt` → the caller leaves the corresponding `m_options_` field).
  return SDDPBoundaryConfig {
      .cuts_file = fc.cuts_file,
      .scale_alpha = fc.scale_alpha,
      .mean_shift = fc.mean_shift,
      .sharing = fc.sharing,
      .mode = fc.mode,
  };
}

const FutureCost* active_future_cost(const PlanningLP& planning_lp)
{
  const auto& sim = planning_lp.simulation();
  if (sim.scene_count() <= 0 || sim.phases().empty()) {
    return nullptr;
  }
  // Read the FutureCost element straight from the System input data
  // (`future_cost_array`), NOT the LP collection: under the SDDP default
  // `low_memory = compress`, `clear_disposable_collections` drops the
  // planning-only FutureCostLP collection (it has no `update_lp`), leaving
  // `elements<FutureCostLP>()` empty even though the input array is intact.
  // The System is replicated across every cell, so (scene 0, phase 0) is
  // authoritative.
  const auto& sys = planning_lp.system(SceneIndex {0}, PhaseIndex {0}).system();
  for (const auto& fc : sys.future_cost_array) {
    // Active unless the `active` field is an explicit scalar `False`.  The
    // FutureCost element is global (no per-stage schedule in practice), so a
    // scalar is the only meaningful form; any vector/file schedule is treated
    // as active (presence == on).
    if (fc.active.has_value()) {
      if (const auto* scalar = std::get_if<IntBool>(&*fc.active);
          scalar != nullptr && *scalar == False)
      {
        continue;  // explicitly deactivated
      }
    }
    return &fc;
  }
  return nullptr;
}

bool has_active_use_user_alpha(const PlanningLP& planning_lp)
{
  const auto* fc = active_future_cost(planning_lp);
  return fc != nullptr && fc->use_user_alpha.value_or(false);
}

std::optional<Uid> active_user_alpha_uid(const PlanningLP& planning_lp) noexcept
{
  const auto* fc = active_future_cost(planning_lp);
  if (fc != nullptr && fc->use_user_alpha.value_or(false)
      && fc->user_alpha_uid.has_value())
  {
    // Return the optional directly (already engaged here) rather than
    // dereferencing + re-wrapping — same value, no redundant round-trip.
    return fc->user_alpha_uid;
  }
  return std::nullopt;
}

}  // namespace gtopt
