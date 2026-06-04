/**
 * @file      sddp_cut_io.cpp
 * @brief     Cut persistence (save/load) for SDDP solver
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements the cut save/load free functions declared in sddp_cut_io.hpp.
 * Extracted from sddp_solver.cpp to reduce file size and improve
 * modularity.  All functions take explicit parameters rather than
 * accessing class members.
 */

#include <algorithm>

#include <gtopt/fmap.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

// ─── UID lookup helpers ─────────────────────────────────────────────────────

[[nodiscard]] auto build_phase_uid_map(const PlanningLP& planning_lp)
    -> flat_map<PhaseUid, PhaseIndex>
{
  const auto& phases = planning_lp.simulation().phases();
  flat_map<PhaseUid, PhaseIndex> phase_map;
  map_reserve(phase_map, phases.size());
  for (auto&& [pi, phase] : enumerate<PhaseIndex>(phases)) {
    phase_map.emplace(phase.uid(), pi);
  }
  return phase_map;
}

[[nodiscard]] auto build_scene_uid_map(const PlanningLP& planning_lp)
    -> flat_map<SceneUid, SceneIndex>
{
  const auto& scenes = planning_lp.simulation().scenes();
  flat_map<SceneUid, SceneIndex> scene_map;
  map_reserve(scene_map, scenes.size());
  for (auto&& [si, scene] : enumerate<SceneIndex>(scenes)) {
    scene_map.emplace(scene.uid(), si);
  }
  return scene_map;
}

// ─── Helper: state-variable name matching ───────────────────────────────────

// ─── Auto-scale alpha helper ────────────────────────────────────────────────

[[nodiscard]] auto effective_scale_alpha(const PlanningLP& planning_lp,
                                         double option_scale_alpha) -> double
{
  if (option_scale_alpha > 0.0) {
    return option_scale_alpha;
  }
  // Auto-scale heuristic (extends SDDPMethod::initialize_solver()):
  //   scale_alpha = max( scale_objective , max(state.var_scale) )
  // across every (scene, phase) state-variable cell.  α carries the future
  // cost (money), so its column scale must cover BOTH regimes it couples:
  // the state-variable scale (the cut's ``wv·efin`` coefficients) AND the
  // objective scale (every other money term α is added to).  Using only the
  // state-var max left α under-scaled against a ``scale_objective = 1e3``
  // objective — an asymmetric gradient magnitude that degrades barrier
  // conditioning; the objective-scale floor keeps α matched to the rest of
  // the objective.  ``scale_objective()`` is 1.0 for SDDP/cascade and under
  // ``--no-scale``, so this only lifts the floor on the scaled monolithic LP.
  const auto& sim = planning_lp.simulation();
  double max_vs = std::max(1.0, planning_lp.options().scale_objective());
  for (auto&& [si, scene] : enumerate<SceneIndex>(sim.scenes())) {
    for (auto&& [pi, phase] : enumerate<PhaseIndex>(sim.phases())) {
      for (const auto& [key, svar] : sim.state_variables(si, pi)) {
        max_vs = std::max(max_vs, svar.var_scale());
      }
    }
  }
  return max_vs;
}
// ``extract_iteration_from_name`` was removed in 2026-05.  Every
// consumer now reads the iteration index directly from the matching
// struct field (``StoredCut::iteration_index``,
// ``RawBoundaryCut::iteration_index``) rather than parsing it back out
// of a generated row label.  See:
//   * ``sddp_cut_parquet.cpp::load_cuts_parquet`` — reads the
//     ``iteration`` int32 column directly.
//   * ``sddp_boundary_cuts.cpp`` / ``sddp_named_cuts.cpp`` — both
//     parsers populate ``iteration_index`` while reading CSV rows
//     and reuse that variable downstream.

}  // namespace gtopt
