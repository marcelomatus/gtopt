/**
 * @file      validate_planning.cpp
 * @brief     Semantic validation of a parsed Planning object
 * @date      Wed Mar 19 00:00:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Implements referential integrity, range, and completeness checks
 * that run after JSON parsing but before LP construction.
 */

#include <cmath>
#include <format>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

#include <gtopt/validate_planning.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Helper: format a SingleId for error messages.
[[nodiscard]] auto format_single_id(const SingleId& sid) -> std::string
{
  return std::visit(
      [](const auto& val) -> std::string
      {
        if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
          return std::format("uid={}", val);
        } else {
          return std::format("name=\"{}\"", val);
        }
      },
      sid);
}

/// Check whether a SingleId matches any element in an array by uid or name.
template<typename Elem>
[[nodiscard]] auto single_id_exists(const SingleId& sid, const Array<Elem>& arr)
    -> bool
{
  return std::visit(
      [&arr](const auto& val) -> bool
      {
        if constexpr (std::is_same_v<std::decay_t<decltype(val)>, Uid>) {
          return std::ranges::any_of(
              arr, [&val](const auto& e) { return e.uid == val; });
        } else {
          return std::ranges::any_of(
              arr, [&val](const auto& e) { return e.name == val; });
        }
      },
      sid);
}

/// Validate that a SingleId field references a valid element.
template<typename Elem>
void check_ref(ValidationResult& result,
               const SingleId& sid,
               const Array<Elem>& arr,
               std::string_view owner_kind,
               std::string_view owner_name,
               std::string_view field_name,
               std::string_view target_kind)
{
  if (!single_id_exists(sid, arr)) {
    result.errors.push_back(
        std::format("{} '{}': {} references non-existent {} ({})",
                    owner_kind,
                    owner_name,
                    field_name,
                    target_kind,
                    format_single_id(sid)));
  }
}

/// Validate referential integrity of all components.
void check_referential_integrity(ValidationResult& result, const System& sys)
{
  // Generator.bus -> Bus
  for (const auto& gen : sys.generator_array) {
    check_ref(
        result, gen.bus, sys.bus_array, "Generator", gen.name, "bus", "Bus");
  }

  // Demand.bus -> Bus
  for (const auto& dem : sys.demand_array) {
    check_ref(result, dem.bus, sys.bus_array, "Demand", dem.name, "bus", "Bus");
  }

  // Line.bus_a, Line.bus_b -> Bus
  for (const auto& line : sys.line_array) {
    check_ref(
        result, line.bus_a, sys.bus_array, "Line", line.name, "bus_a", "Bus");
    check_ref(
        result, line.bus_b, sys.bus_array, "Line", line.name, "bus_b", "Bus");
  }

  // Turbine.waterway -> Waterway (optional), Turbine.generator -> Generator
  for (const auto& turb : sys.turbine_array) {
    if (turb.waterway.has_value()) {
      check_ref(result,
                turb.waterway.value(),
                sys.waterway_array,
                "Turbine",
                turb.name,
                "waterway",
                "Waterway");
    }
    check_ref(result,
              turb.generator,
              sys.generator_array,
              "Turbine",
              turb.name,
              "generator",
              "Generator");
  }

  // Flow.junction -> Junction (optional in flow-turbine mode)
  for (const auto& flow : sys.flow_array) {
    if (flow.junction.has_value()) {
      check_ref(result,
                flow.junction.value(),
                sys.junction_array,
                "Flow",
                flow.name,
                "junction",
                "Junction");
    }
  }

  // Waterway.junction_a, junction_b -> Junction
  for (const auto& ww : sys.waterway_array) {
    check_ref(result,
              ww.junction_a,
              sys.junction_array,
              "Waterway",
              ww.name,
              "junction_a",
              "Junction");
    check_ref(result,
              ww.junction_b,
              sys.junction_array,
              "Waterway",
              ww.name,
              "junction_b",
              "Junction");
  }

  // Converter.battery -> Battery, generator -> Generator, demand -> Demand
  for (const auto& conv : sys.converter_array) {
    check_ref(result,
              conv.battery,
              sys.battery_array,
              "Converter",
              conv.name,
              "battery",
              "Battery");
    check_ref(result,
              conv.generator,
              sys.generator_array,
              "Converter",
              conv.name,
              "generator",
              "Generator");
    check_ref(result,
              conv.demand,
              sys.demand_array,
              "Converter",
              conv.name,
              "demand",
              "Demand");
  }

  // Reservoir.junction -> Junction
  for (const auto& res : sys.reservoir_array) {
    check_ref(result,
              res.junction,
              sys.junction_array,
              "Reservoir",
              res.name,
              "junction",
              "Junction");
  }
}

/// Validate range constraints on simulation parameters.
void check_ranges(ValidationResult& result, const Planning& planning)
{
  const auto& sim = planning.simulation;

  // Block duration > 0
  for (const auto& blk : sim.block_array) {
    if (blk.duration <= 0.0) {
      result.errors.push_back(std::format(
          "Block uid={}: duration ({}) must be > 0", blk.uid, blk.duration));
    }
  }

  // Stage count_block > 0
  for (const auto& stg : sim.stage_array) {
    if (stg.count_block == 0) {
      result.errors.push_back(
          std::format("Stage uid={}: count_block must be > 0", stg.uid));
    }
  }

  // Generator capacity non-negative (warning only)
  for (const auto& gen : planning.system.generator_array) {
    if (gen.capacity.has_value()) {
      const auto& cap = gen.capacity.value();
      if (std::holds_alternative<Real>(cap) && std::get<Real>(cap) < 0.0) {
        result.warnings.push_back(
            std::format("Generator '{}': capacity ({}) is negative",
                        gen.name,
                        std::get<Real>(cap)));
      }
    }
  }
}

/// Validate structural completeness.
void check_completeness(ValidationResult& result, const Planning& planning)
{
  if (planning.system.bus_array.empty()) {
    result.errors.emplace_back("System has no buses defined");
  }

  if (planning.simulation.block_array.empty()) {
    result.errors.emplace_back("Simulation has no blocks defined");
  }

  if (planning.simulation.stage_array.empty()) {
    result.errors.emplace_back("Simulation has no stages defined");
  }
}

/// Tolerance for probability sum comparison.
constexpr double prob_tolerance = 1e-6;

/// Check and optionally rescale scenario probabilities per scene.
void check_scenario_probabilities(ValidationResult& result, Planning& planning)
{
  const auto mode = planning.simulation.probability_rescale.value_or(
      ProbabilityRescaleMode::runtime);
  const bool do_rescale = (mode != ProbabilityRescaleMode::none);

  auto& scenarios = planning.simulation.scenario_array;
  const auto& scenes = planning.simulation.scene_array;

  if (scenarios.empty()) {
    return;
  }

  // When there are no explicit scenes, all scenarios form one implicit scene.
  // Check that the total probability sums to 1.0.
  if (scenes.empty()) {
    double total = 0.0;
    for (const auto& sc : scenarios) {
      total += sc.probability_factor.value_or(1.0);
    }

    if (std::abs(total - 1.0) > prob_tolerance) {
      result.warnings.push_back(std::format(
          "Scenario probability_factor values sum to {:.6f} (expected 1.0)",
          total));

      if (do_rescale && total > 0.0) {
        for (auto& sc : scenarios) {
          const double p = sc.probability_factor.value_or(1.0);
          sc.probability_factor = p / total;
        }
        result.warnings.push_back(std::format(
            "Rescaled {} scenario probability_factor values to sum 1.0",
            scenarios.size()));
      }
    }
    return;
  }

  // Per-scene: check that scenario probabilities within each scene sum to 1.0.
  // Skip scenes with a single scenario — they are trivially normalised and
  // their probability_factor represents the global weight used for
  // cross-scene aggregation (e.g. SDDP with one scenario per scene).
  for (const auto& scene : scenes) {
    const auto first = scene.first_scenario;
    const auto count = (scene.count_scenario == std::dynamic_extent)
        ? (scenarios.size() - first)
        : scene.count_scenario;

    if (first + count > scenarios.size()) {
      result.errors.push_back(std::format(
          "Scene '{}': first_scenario ({}) + count_scenario ({}) exceeds "
          "scenario_array size ({})",
          scene.name.value_or("?"),
          first,
          count,
          scenarios.size()));
      continue;
    }

    // Single-scenario scenes need no intra-scene normalisation.
    if (count <= 1) {
      continue;
    }

    double total = 0.0;
    for (std::size_t i = first; i < first + count; ++i) {
      total += scenarios[i].probability_factor.value_or(1.0);
    }

    if (std::abs(total - 1.0) > prob_tolerance) {
      result.warnings.push_back(std::format(
          "Scene '{}': scenario probability_factor values sum to {:.6f} "
          "(expected 1.0)",
          scene.name.value_or("?"),
          total));

      if (do_rescale && total > 0.0) {
        for (std::size_t i = first; i < first + count; ++i) {
          const double p = scenarios[i].probability_factor.value_or(1.0);
          scenarios[i].probability_factor = p / total;
        }
        result.warnings.push_back(std::format(
            "Rescaled scenario probabilities in scene '{}' to sum 1.0",
            scene.name.value_or("?")));
      }
    }
  }

  // Check that scene-level probability totals sum to 1.0 across all scenes.
  double scene_total = 0.0;
  for (const auto& scene : scenes) {
    const auto first = scene.first_scenario;
    const auto count = (scene.count_scenario == std::dynamic_extent)
        ? (scenarios.size() - first)
        : scene.count_scenario;
    if (first + count > scenarios.size()) {
      continue;  // already reported above
    }
    for (std::size_t i = first; i < first + count; ++i) {
      scene_total += scenarios[i].probability_factor.value_or(1.0);
    }
  }

  if (std::abs(scene_total - 1.0) > prob_tolerance) {
    result.warnings.push_back(
        std::format("Total scene probability sums to {:.6f} across {} scenes "
                    "(expected 1.0)",
                    scene_total,
                    scenes.size()));

    if (do_rescale && scene_total > 0.0) {
      for (const auto& scene : scenes) {
        const auto first = scene.first_scenario;
        const auto count = (scene.count_scenario == std::dynamic_extent)
            ? (scenarios.size() - first)
            : scene.count_scenario;
        if (first + count > scenarios.size()) {
          continue;
        }
        for (std::size_t i = first; i < first + count; ++i) {
          const double p = scenarios[i].probability_factor.value_or(1.0);
          scenarios[i].probability_factor = p / scene_total;
        }
      }
      result.warnings.push_back(
          std::format("Rescaled all scenario probabilities across {} scenes to "
                      "sum 1.0",
                      scenes.size()));
    }
  }
}

}  // namespace

[[nodiscard]] ValidationResult validate_planning(Planning& planning)
{
  ValidationResult result;

  check_referential_integrity(result, planning.system);
  check_ranges(result, planning);
  check_completeness(result, planning);
  check_scenario_probabilities(result, planning);

  // Log all findings
  for (const auto& warn : result.warnings) {
    spdlog::warn("Validation: {}", warn);
  }
  for (const auto& err : result.errors) {
    spdlog::error("Validation: {}", err);
  }

  if (result.ok()) {
    spdlog::info("Planning validation passed");
  } else {
    spdlog::error("Planning validation failed with {} error(s)",
                  result.errors.size());
  }

  return result;
}

}  // namespace gtopt
