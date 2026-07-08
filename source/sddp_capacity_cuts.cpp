/**
 * @file      sddp_capacity_cuts.cpp
 * @brief     Capacity-space projection of stored SDDP Benders cuts —
 *            implementation.
 * @date      2026-07-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <optional>

#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/sddp_capacity_cuts.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// True when @p col_name is a capacity accounting state — the
/// `capainst` / `capacost` names `CapacityObjectBase` registers via
/// `add_state_col` (the state-variable keys use the un-suffixed names;
/// the `_prev` variants are LP column metadata only).
[[nodiscard]] bool is_capacity_state_name(std::string_view col_name) noexcept
{
  return col_name == CapacityObjectBase::CapainstName
      || col_name == CapacityObjectBase::CapacostName;
}

}  // namespace

auto extract_capacity_cuts(const SimulationLP& sim,
                           std::span<const StoredCut> cuts,
                           PhaseIndex phase_index) -> std::vector<CapacityCut>
{
  std::vector<CapacityCut> out;
  const auto wanted_phase_uid = sim.uid_of(phase_index);

  for (const auto& sc : cuts) {
    if (sc.type != CutType::Optimality || sc.phase_uid != wanted_phase_uid) {
      continue;
    }

    // Resolve the originating scene (per-scene registries hold the
    // column universe the stored indices refer to).
    std::optional<SceneIndex> scene;
    for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
      if (sim.uid_of(si) == sc.scene_uid) {
        scene = si;
        break;
      }
    }
    if (!scene.has_value()) {
      SPDLOG_WARN(
          "extract_capacity_cuts: cut scene_uid {} not found in the "
          "simulation — cut skipped",
          sc.scene_uid);
      continue;
    }

    CapacityCut cut {
        .scene_uid = sc.scene_uid,
        .phase_uid = sc.phase_uid,
        .iteration = sc.iteration_index,
        .rhs = sc.rhs,
    };

    const auto& svars = sim.state_variables(*scene, phase_index);
    bool unresolved = false;
    for (const auto& [col, coeff] : sc.coefficients) {
      const StateVariable::Key* matched_key = nullptr;
      for (const auto& [key, svar] : svars) {
        if (svar.col() == col) {
          matched_key = &key;
          break;
        }
      }
      if (matched_key == nullptr) {
        // A stored coefficient outside this run's registry universe —
        // e.g. a cut reloaded against a different model (precondition
        // A6 of the cut-validity document).  The projection cannot be
        // trusted; skip the whole cut.
        SPDLOG_WARN(
            "extract_capacity_cuts: coefficient column {} of cut "
            "(scene {}, phase {}, iter {}) resolves to no state "
            "variable — cut skipped",
            static_cast<int>(col),
            sc.scene_uid,
            sc.phase_uid,
            gtopt::uid_of(sc.iteration_index));
        unresolved = true;
        break;
      }
      if (matched_key->class_name == sddp_alpha_class_name) {
        continue;  // the α / varphi_s coefficient — not a state coord
      }
      if (is_capacity_state_name(matched_key->col_name)) {
        cut.coefficients.push_back(CapacityCutCoefficient {
            .class_name = matched_key->class_name,
            .col_name = matched_key->col_name,
            .uid = matched_key->uid,
            .scenario_uid = matched_key->scenario_uid,
            .stage_uid = matched_key->stage_uid,
            .coeff = coeff,
        });
      } else {
        ++cut.dropped_state_coefficients;
      }
    }
    if (unresolved) {
      continue;
    }

    out.push_back(std::move(cut));
  }

  // Function-form spdlog: the SPDLOG_DEBUG macro is compiled out under
  // the INFO-baked PCH; this summary must stay runtime-recoverable.
  spdlog::debug(
      "extract_capacity_cuts: {} capacity-space cut(s) extracted for "
      "phase index {} from {} stored cut(s)",
      out.size(),
      static_cast<int>(phase_index),
      cuts.size());
  return out;
}

}  // namespace gtopt
