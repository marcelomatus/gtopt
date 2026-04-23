/**
 * @file      system.cpp
 * @brief     Header of System class methods
 * @date      Sun Mar 30 16:04:21 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the methods of the System class, which represents the
 * core data.
 */

#include <algorithm>
#include <set>

#include <gtopt/bus_island.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace
{
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

/**
 * @brief Build a set of UIDs from an element array
 * @tparam T Element type (must have .uid member)
 * @param arr Array of elements
 * @return Set of existing UIDs
 */
template<typename T>
[[nodiscard]] auto build_uid_set(const gtopt::Array<T>& arr)
    -> std::set<gtopt::Uid>
{
  std::set<gtopt::Uid> result;
  for (const auto& elem : arr) {
    result.insert(elem.uid);
  }
  return result;
}

/**
 * @brief Build a set of names from an element array
 * @tparam T Element type (must have .name member)
 * @param arr Array of elements
 * @return Set of existing names
 */
template<typename T>
[[nodiscard]] auto build_name_set(const gtopt::Array<T>& arr)
    -> std::set<std::string>
{
  std::set<std::string> result;
  for (const auto& elem : arr) {
    result.insert(elem.name);
  }
  return result;
}

}  // namespace

namespace gtopt
{

void System::setup_reference_bus(const PlanningOptionsLP& options)
{
  detect_islands_and_fix_references(bus_array, line_array, options);
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
        SPDLOG_WARN(
            "Battery '{}': source_generator '{}' not found in generator_array",
            battery.name,
            src_label);
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

void System::expand_reservoir_constraints()
{
  auto seep_uid = next_uid(reservoir_seepage_array);
  auto dlim_uid = next_uid(reservoir_discharge_limit_array);
  auto pfac_uid = next_uid(reservoir_production_factor_array);

  // Build sets of existing UIDs/names for O(1) duplicate detection
  auto seep_uids = build_uid_set(reservoir_seepage_array);
  auto seep_names = build_name_set(reservoir_seepage_array);
  auto dlim_uids = build_uid_set(reservoir_discharge_limit_array);
  auto dlim_names = build_name_set(reservoir_discharge_limit_array);
  auto pfac_uids = build_uid_set(reservoir_production_factor_array);
  auto pfac_names = build_name_set(reservoir_production_factor_array);

  for (auto& rsv : reservoir_array) {
    const SingleId rsv_id {rsv.uid};

    for (auto& s : rsv.seepage) {
      if (s.uid == unknown_uid || seep_uids.contains(s.uid)) {
        s.uid = seep_uid++;
      }
      if (s.name.empty() || seep_names.contains(s.name)) {
        s.name = rsv.name + "_seepage_" + std::to_string(s.uid);
      }
      s.reservoir = rsv_id;
      seep_uids.insert(s.uid);
      seep_names.insert(s.name);
      reservoir_seepage_array.push_back(std::move(s));
    }
    rsv.seepage.clear();

    for (auto& d : rsv.discharge_limit) {
      if (d.uid == unknown_uid || dlim_uids.contains(d.uid)) {
        d.uid = dlim_uid++;
      }
      if (d.name.empty() || dlim_names.contains(d.name)) {
        d.name = rsv.name + "_dlim_" + std::to_string(d.uid);
      }
      d.reservoir = rsv_id;
      dlim_uids.insert(d.uid);
      dlim_names.insert(d.name);
      reservoir_discharge_limit_array.push_back(std::move(d));
    }
    rsv.discharge_limit.clear();

    for (auto& p : rsv.production_factor) {
      if (p.uid == unknown_uid || pfac_uids.contains(p.uid)) {
        p.uid = pfac_uid++;
      }
      if (p.name.empty() || pfac_names.contains(p.name)) {
        p.name = rsv.name + "_pfac_" + std::to_string(p.uid);
      }
      p.reservoir = rsv_id;
      pfac_uids.insert(p.uid);
      pfac_names.insert(p.name);
      reservoir_production_factor_array.push_back(std::move(p));
    }
    rsv.production_factor.clear();
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
  gtopt::merge(lng_terminal_array, std::move(sys.lng_terminal_array));

  gtopt::merge(reserve_zone_array, std::move(sys.reserve_zone_array));
  gtopt::merge(reserve_provision_array, std::move(sys.reserve_provision_array));
  gtopt::merge(commitment_array, std::move(sys.commitment_array));
  gtopt::merge(simple_commitment_array, std::move(sys.simple_commitment_array));

  gtopt::merge(inertia_zone_array, std::move(sys.inertia_zone_array));
  gtopt::merge(inertia_provision_array, std::move(sys.inertia_provision_array));

  gtopt::merge(junction_array, std::move(sys.junction_array));
  gtopt::merge(waterway_array, std::move(sys.waterway_array));
  gtopt::merge(flow_array, std::move(sys.flow_array));
  gtopt::merge(reservoir_array, std::move(sys.reservoir_array));
  gtopt::merge(reservoir_seepage_array, std::move(sys.reservoir_seepage_array));
  gtopt::merge(reservoir_discharge_limit_array,
               std::move(sys.reservoir_discharge_limit_array));
  gtopt::merge(turbine_array, std::move(sys.turbine_array));
  gtopt::merge(pump_array, std::move(sys.pump_array));
  gtopt::merge(reservoir_production_factor_array,
               std::move(sys.reservoir_production_factor_array));

  gtopt::merge(flow_right_array, std::move(sys.flow_right_array));
  gtopt::merge(volume_right_array, std::move(sys.volume_right_array));

  gtopt::merge(user_param_array, std::move(sys.user_param_array));
  gtopt::merge(user_constraint_array, std::move(sys.user_constraint_array));

  if (sys.user_constraint_file.has_value()) {
    user_constraint_file = std::move(sys.user_constraint_file);
  }

  if (!sys.user_constraint_files.empty()) {
    for (auto& f : sys.user_constraint_files) {
      user_constraint_files.push_back(std::move(f));
    }
  }
}

}  // namespace gtopt
