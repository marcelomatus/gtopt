/**
 * @file      system.cpp<gtopt>
 * @brief     Header of System class methods
 * @date      Sun Mar 30 16:04:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the methods of the System class, which represents the
 * core data.
 */

#include <algorithm>

#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace
{
/**
 * @brief Determines if the system needs a reference bus for voltage angle
 *
 * This function checks if the system requires setting a reference bus with a
 * fixed voltage angle (theta) for power flow calculations. A reference bus is
 * needed if:
 * - There are multiple buses
 * - Single-bus mode is not active
 * - Kirchhoff's laws are being used
 * - No bus has already been designated as a reference
 * - At least one bus needs Kirchhoff constraints based on threshold
 *
 * @tparam BusContainer Type of container holding buses
 * @tparam OptionsType Type of system options
 * @param buses Container of system buses
 * @param options System options
 * @return True if a reference bus needs to be set, false otherwise
 */
template<typename BusContainer, typename OptionsType>
constexpr bool needs_ref_theta(const BusContainer& buses,
                               const OptionsType& options)
{
  // Early return conditions
  if (buses.size() <= 1 || options.use_single_bus() || !options.use_kirchhoff())
  {
    return false;
  }

  // Check if any bus already has reference theta set
  const bool has_reference_bus = std::ranges::any_of(
      buses, [](const auto& bus) { return bus.reference_theta.has_value(); });

  if (has_reference_bus) {
    return false;
  }

  // Check if any bus needs Kirchhoff according to the threshold
  const auto kirchhoff_threshold = options.kirchhoff_threshold();
  return std::ranges::any_of(
      buses,
      [kirchhoff_threshold](const auto& bus)
      { return bus.needs_kirchhoff(kirchhoff_threshold); });
}

/**
 * @brief Returns the next available UID for an element array
 * @tparam T Element type (must have .uid member)
 * @param arr Array of elements
 * @return One past the maximum existing UID, or 1 if array is empty
 */
template<typename T>
[[nodiscard]] auto next_uid(const gtopt::Array<T>& arr) -> gtopt::Uid
{
  if (arr.empty()) {
    return 1;
  }
  auto max_it = std::ranges::max_element(
      arr, {}, [](const auto& elem) { return elem.uid; });
  return max_it->uid + 1;
}

}  // namespace

namespace gtopt
{

void System::setup_reference_bus(const OptionsLP& options)
{
  if (needs_ref_theta(bus_array, options)) {
    auto& bus = bus_array.front();
    bus.reference_theta = 0.0;
    SPDLOG_TRACE(std::format(
        "Setting bus '{}' as reference bus (reference_theta=0)", bus.name));
  }
}

void System::expand_batteries()
{
  // Compute starting UIDs for auto-generated elements.
  // These must be computed before appending to avoid shifting during iteration.
  auto gen_uid = next_uid(generator_array);
  auto dem_uid = next_uid(demand_array);
  auto conv_uid = next_uid(converter_array);
  auto bus_uid = next_uid(bus_array);

  for (auto& battery : battery_array) {
    if (!battery.bus.has_value()) {
      continue;  // traditional multi-element definition — skip
    }

    const auto gen_name = battery.name + "_gen";
    const auto dem_name = battery.name + "_dem";
    const auto conv_name = battery.name + "_conv";

    // Determine charge bus: internal (coupled mode) or external (standalone)
    SingleId charge_bus = *battery.bus;

    if (battery.source_generator.has_value()) {
      // Generation-coupled mode: create internal bus for the charge path
      const auto int_bus_name = battery.name + "_int_bus";
      bus_array.push_back(Bus {
          .uid = bus_uid,
          .name = int_bus_name,
      });
      charge_bus = SingleId {Uid {bus_uid}};
      bus_uid++;

      // Find the referenced source generator and set its bus to the internal
      // bus, overwriting any previously set bus value.
      const auto src_id = *battery.source_generator;  // captured by value
      auto it =
          std::ranges::find_if(generator_array,
                               [src_id](const Generator& g)
                               {
                                 if (std::holds_alternative<Uid>(src_id)) {
                                   return g.uid == std::get<Uid>(src_id);
                                 }
                                 return g.name == std::get<Name>(src_id);
                               });
      if (it != generator_array.end()) {
        it->bus = charge_bus;
      } else {
        const auto src_label = std::visit(
            [](const auto& v) { return std::format("{}", v); }, src_id);
        SPDLOG_WARN(std::format(
            "Battery '{}': source_generator '{}' not found in generator_array",
            battery.name,
            src_label));
      }
      battery.source_generator.reset();
    }

    // Discharge generator: power injected into the external bus
    generator_array.push_back(Generator {
        .uid = gen_uid++,
        .name = gen_name,
        .bus = *battery.bus,
        .gcost = battery.gcost,
        .capacity = battery.pmax_discharge,
    });

    // Charge demand: power absorbed from the charge bus
    demand_array.push_back(Demand {
        .uid = dem_uid++,
        .name = dem_name,
        .bus = charge_bus,
        .capacity = battery.pmax_charge,
    });

    // Converter linking battery, generator, and demand
    converter_array.push_back(Converter {
        .uid = conv_uid++,
        .name = conv_name,
        .battery = Name {battery.name},
        .generator = Name {gen_name},
        .demand = Name {dem_name},
    });

    SPDLOG_TRACE(
        std::format("Expanded battery '{}': gen='{}' dem='{}' conv='{}'",
                    battery.name,
                    gen_name,
                    dem_name,
                    conv_name));

    // Clear bus so re-expansion is idempotent
    battery.bus.reset();
  }
}

void System::merge(System&& sys)  // NOLINT
{
  if (!sys.name.empty()) {
    name = std::move(sys.name);
  }

  if (!sys.version.empty()) {
    version = std::move(sys.version);
  }

  gtopt::merge(bus_array, std::move(sys.bus_array));
  gtopt::merge(demand_array, std::move(sys.demand_array));
  gtopt::merge(generator_array, std::move(sys.generator_array));
  gtopt::merge(line_array, std::move(sys.line_array));

  gtopt::merge(generator_profile_array, std::move(sys.generator_profile_array));
  gtopt::merge(demand_profile_array, std::move(sys.demand_profile_array));

  gtopt::merge(battery_array, std::move(sys.battery_array));
  gtopt::merge(converter_array, std::move(sys.converter_array));

  gtopt::merge(reserve_zone_array, std::move(sys.reserve_zone_array));
  gtopt::merge(reserve_provision_array, std::move(sys.reserve_provision_array));

  gtopt::merge(junction_array, std::move(sys.junction_array));
  gtopt::merge(waterway_array, std::move(sys.waterway_array));
  gtopt::merge(flow_array, std::move(sys.flow_array));
  gtopt::merge(reservoir_array, std::move(sys.reservoir_array));
  gtopt::merge(filtration_array, std::move(sys.filtration_array));
  gtopt::merge(turbine_array, std::move(sys.turbine_array));
  gtopt::merge(reservoir_efficiency_array,
               std::move(sys.reservoir_efficiency_array));

  gtopt::merge(user_constraint_array, std::move(sys.user_constraint_array));

  if (sys.user_constraint_file.has_value()) {
    user_constraint_file = std::move(sys.user_constraint_file);
  }
}

}  // namespace gtopt
