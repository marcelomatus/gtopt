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
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtopt/field_sched.hpp>
#include <gtopt/utils.hpp>
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

  // Pump.waterway -> Waterway, Pump.demand -> Demand
  for (const auto& pump : sys.pump_array) {
    check_ref(result,
              pump.waterway,
              sys.waterway_array,
              "Pump",
              pump.name,
              "waterway",
              "Waterway");
    check_ref(result,
              pump.demand,
              sys.demand_array,
              "Pump",
              pump.name,
              "demand",
              "Demand");
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

// ── Positivity helpers ────────────────────────────────────────────────
//
// Water-right, line, and waterway magnitudes must be physically
// positive (zero allowed when explicitly called out; negative is
// always a schema bug).  These helpers walk the FieldSched variant
// (scalar / vector / file) and report any negative scalar value
// found.  FileSched entries are skipped — they resolve at
// load-time and the file loader is responsible for its own checks.

enum class Positivity : std::uint8_t
{
  strict,  ///< value must be > 0
  non_negative,  ///< value must be >= 0
};

[[nodiscard]] constexpr bool violates(double v, Positivity p) noexcept
{
  return p == Positivity::strict ? !(v > 0.0) : !(v >= 0.0);
}

[[nodiscard]] constexpr std::string_view positivity_name(Positivity p) noexcept
{
  return p == Positivity::strict ? "> 0" : ">= 0";
}

template<typename Vec>
void check_vector_positive(ValidationResult& result,
                           const Vec& values,
                           Positivity p,
                           std::string_view owner_kind,
                           std::string_view owner_name,
                           std::string_view field_name)
{
  if constexpr (std::is_arithmetic_v<
                    std::remove_cvref_t<decltype(values.front())>>)
  {
    for (const auto& [i, v_raw] : enumerate(values)) {
      const auto v = static_cast<double>(v_raw);
      if (violates(v, p)) {
        result.errors.push_back(std::format("{} '{}': {}[{}] = {} must be {}",
                                            owner_kind,
                                            owner_name,
                                            field_name,
                                            i,
                                            v,
                                            positivity_name(p)));
        return;  // one error per field is enough
      }
    }
  } else {
    // Nested vector (TBReal / STBReal).  Recurse one level.
    for (const auto& sub : values) {
      check_vector_positive(result, sub, p, owner_kind, owner_name, field_name);
    }
  }
}

/// Check a FieldSched<Real, ...> variant for positivity.
/// FileSched (string) entries are skipped — those resolve at load-time.
template<typename FieldSched>
void check_field_positive(ValidationResult& result,
                          const FieldSched& fs,
                          Positivity p,
                          std::string_view owner_kind,
                          std::string_view owner_name,
                          std::string_view field_name)
{
  std::visit(
      [&](const auto& val)
      {
        using T = std::remove_cvref_t<decltype(val)>;
        if constexpr (std::is_same_v<T, Real>) {
          if (violates(static_cast<double>(val), p)) {
            result.errors.push_back(std::format("{} '{}': {} = {} must be {}",
                                                owner_kind,
                                                owner_name,
                                                field_name,
                                                val,
                                                positivity_name(p)));
          }
        } else if constexpr (std::is_same_v<T, FileSched>) {
          // Filename reference — defer validation to load-time.
        } else {
          check_vector_positive(
              result, val, p, owner_kind, owner_name, field_name);
        }
      },
      fs);
}

/// Check an optional FieldSched field.  Skips when unset.
template<typename OptFieldSched>
void check_opt_field_positive(ValidationResult& result,
                              const OptFieldSched& opt,
                              Positivity p,
                              std::string_view owner_kind,
                              std::string_view owner_name,
                              std::string_view field_name)
{
  if (!opt.has_value()) {
    return;
  }
  check_field_positive(result, *opt, p, owner_kind, owner_name, field_name);
}

/// Check positivity of flow-right / volume-right / line / waterway fields.
void check_positivity(ValidationResult& result, const System& sys)
{
  // FlowRight: discharge and fmax must be >= 0 (consumptive flow).
  for (const auto& fr : sys.flow_right_array) {
    check_field_positive(result,
                         fr.discharge,
                         Positivity::non_negative,
                         "FlowRight",
                         fr.name,
                         "discharge");
    check_opt_field_positive(result,
                             fr.fmax,
                             Positivity::non_negative,
                             "FlowRight",
                             fr.name,
                             "fmax");
  }

  // VolumeRight: storage and demand magnitudes must be >= 0.
  for (const auto& vr : sys.volume_right_array) {
    check_opt_field_positive(result,
                             vr.emin,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "emin");
    check_opt_field_positive(result,
                             vr.emax,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "emax");
    check_opt_field_positive(result,
                             vr.demand,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "demand");
    check_opt_field_positive(result,
                             vr.fmax,
                             Positivity::non_negative,
                             "VolumeRight",
                             vr.name,
                             "fmax");
    if (vr.eini.has_value() && *vr.eini < 0.0) {
      result.errors.push_back(std::format(
          "VolumeRight '{}': eini = {} must be >= 0", vr.name, *vr.eini));
    }
    if (vr.efin.has_value() && *vr.efin < 0.0) {
      result.errors.push_back(std::format(
          "VolumeRight '{}': efin = {} must be >= 0", vr.name, *vr.efin));
    }
  }

  // Line: transfer capacities must be >= 0 (direction is encoded
  // separately in bus_a/bus_b).
  for (const auto& line : sys.line_array) {
    check_opt_field_positive(result,
                             line.tmax_ab,
                             Positivity::non_negative,
                             "Line",
                             line.name,
                             "tmax_ab");
    check_opt_field_positive(result,
                             line.tmax_ba,
                             Positivity::non_negative,
                             "Line",
                             line.name,
                             "tmax_ba");
  }

  // Waterway (hydro flow): fmin and fmax must be >= 0.  Negative flow
  // would invert the direction, which should be expressed by swapping
  // junction_a/junction_b instead.  fmax == 0 is legal: it means the
  // flow is pinned at 0 (matches PLP's VertMax=0 → qv_k ∈ [0,0] on
  // the vertimiento waterway — see plpcnfce.dat + leecnfce.f:342-343).
  for (const auto& ww : sys.waterway_array) {
    check_opt_field_positive(
        result, ww.fmin, Positivity::non_negative, "Waterway", ww.name, "fmin");
    check_opt_field_positive(
        result, ww.fmax, Positivity::non_negative, "Waterway", ww.name, "fmax");
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
  check_positivity(result, planning.system);
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
