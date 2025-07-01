/**
 * @file      system_lp.cpp
 * @brief     Implementation of SystemLP class for power system LP formulation
 * @date      Tue Apr  8 01:20:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the SystemLP class, which is responsible for creating
 * and managing the linear programming formulation of power system planning
 * problems. It handles conversion of system components to their LP
 * representations, coordinates constraint generation across the system, and
 * provides utilities for adding constraints to the linear problem and
 * extracting results.
 */

#include <algorithm>
#include <cstdlib>

#include <gtopt/linear_interface.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>
#include <range/v3/view/all.hpp>
#include <spdlog/spdlog.h>

#include "gtopt/options_lp.hpp"
#include "gtopt/simulation_lp.hpp"
#include "gtopt/system_context.hpp"

namespace
{
using namespace gtopt;

/**
 * @brief Creates a collection of LP elements from system elements
 *
 * This function transforms a vector of system elements into a collection
 * of their corresponding LP representations.
 *
 * @tparam Out Output LP element type
 * @tparam Inp Input system element type
 * @tparam InputContext Type of input context
 * @param input_context Context for input processing
 * @param input Vector of system elements to transform
 * @return Collection of LP elements
 */
template<typename Out, typename Inp, typename InputContext>
constexpr auto make_collection(InputContext& input_context,
                               const std::vector<Inp>& input) -> Collection<Out>
{
  return Collection<Out> {
      input
      | ranges::views::transform(
          [&](auto element) { return Out {input_context, std::move(element)}; })
      | ranges::to<std::vector<Out>>()};
}

/**
 * @brief Creates a collection of LP elements from optional system elements
 *
 * Overload for optional vector input that handles the case when the input
 * vector might not be present.
 *
 * @tparam Out Output LP element type
 * @tparam Inp Input system element type
 * @tparam InputContext Type of input context
 * @param input_context Context for input processing
 * @param input Optional vector of system elements
 * @return Collection of LP elements or empty collection if input is empty
 */
template<typename Out, typename Inp, typename InputContext>
constexpr auto make_collection(InputContext& input_context,
                               const std::optional<std::vector<Inp>>& input)
    -> Collection<Out>
{
  if (input) [[likely]] {
    return make_collection<Out>(input_context, *input);
  }
  return Collection<Out> {};
}

//
//
//

constexpr void add_to_lp(auto& collections,
                         SystemContext& system_context,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp)
{
  const bool use_single_bus = system_context.options().use_single_bus();

  auto visitor = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, BusLP>) {
      return !use_single_bus || system_context.system().is_single_bus(e.id())
          ? e.add_to_lp(system_context, scenario, stage, lp)
          : true;
    } else if constexpr (std::is_same_v<T, LineLP>) {
      return !use_single_bus ? e.add_to_lp(system_context, scenario, stage, lp)
                             : true;
    } else {
      return e.add_to_lp(system_context, scenario, stage, lp);
    }
  };

  visit_elements(collections, visitor);
}

constexpr auto create_linear_interface(auto& collections,
                                       SystemContext& system_context,
                                       const PhaseLP& phase,
                                       const SceneLP& scene,
                                       const auto& flat_opts)
{
  LinearProblem lp;
  // Process all active stages in phase
  for (auto&& stage : phase.stages()) {
    // Process all active scenarios in simulation
    for (auto&& scenario : scene.scenarios()) {
      add_to_lp(collections, system_context, scenario, stage, lp);
    }
  }

  // Convert and store the flattened LP representation

  return LinearInterface {lp.to_flat(flat_opts)};
}

constexpr auto create_collections(const auto& system_context, const auto& sys)
{
  InputContext ic(system_context);
  SystemLP::collections_t colls;

  std::get<Collection<BusLP>>(colls) =
      make_collection<BusLP>(ic, sys.bus_array);
  std::get<Collection<DemandLP>>(colls) =
      make_collection<DemandLP>(ic, sys.demand_array);
  std::get<Collection<GeneratorLP>>(colls) =
      make_collection<GeneratorLP>(ic, sys.generator_array);
  std::get<Collection<LineLP>>(colls) =
      make_collection<LineLP>(ic, sys.line_array);
  std::get<Collection<GeneratorProfileLP>>(colls) =
      make_collection<GeneratorProfileLP>(ic, sys.generator_profile_array);
  std::get<Collection<DemandProfileLP>>(colls) =
      make_collection<DemandProfileLP>(ic, sys.demand_profile_array);
  std::get<Collection<BatteryLP>>(colls) =
      make_collection<BatteryLP>(ic, sys.battery_array);
  std::get<Collection<ConverterLP>>(colls) =
      make_collection<ConverterLP>(ic, sys.converter_array);
  std::get<Collection<ReserveZoneLP>>(colls) =
      make_collection<ReserveZoneLP>(ic, sys.reserve_zone_array);
  std::get<Collection<ReserveProvisionLP>>(colls) =
      make_collection<ReserveProvisionLP>(ic, sys.reserve_provision_array);

#ifdef GTOPT_EXTRA
  std::get<Collection<JunctionLP>>(colls) =
      make_collection<JunctionLP>(ic, sys.junctions);
  std::get<Collection<WaterwayLP>>(colls) =
      make_collection<WaterwayLP>(ic, sys.waterways);
  std::get<Collection<InflowLP>>(colls) =
      make_collection<InflowLP>(ic, sys.inflows);
  std::get<Collection<OutflowLP>>(colls) =
      make_collection<OutflowLP>(ic, sys.outflows);
  std::get<Collection<ReservoirLP>>(colls) =
      make_collection<ReservoirLP>(ic, sys.reservoirs);
  std::get<Collection<FiltrationLP>>(colls) =
      make_collection<FiltrationLP>(ic, sys.filtrations);
  std::get<Collection<TurbineLP>>(colls) =
      make_collection<TurbineLP>(ic, sys.turbines);
  std::get<Collection<EmissionZoneLP>>(colls) =
      make_collection<EmissionZoneLP>(ic, sys.emission_zones);
  std::get<Collection<GeneratorEmissionLP>>(colls) =
      make_collection<GeneratorEmissionLP>(ic, sys.generator_emissions);
  std::get<Collection<DemandEmissionLP>>(colls) =
      make_collection<DemandEmissionLP>(ic, sys.demand_emissions);
#endif

  return colls;
}

}  // namespace

namespace gtopt
{
void SystemLP::create_lp(const FlatOptions& flat_opts)
{
  m_linear_interface_ = create_linear_interface(
      collections(), system_context(), phase(), scene(), flat_opts);
}

SystemLP::SystemLP(const System& system,
                   SimulationLP& simulation,
                   PhaseLP phase,
                   SceneLP scene,
                   const FlatOptions& flat_opts)
    : m_system_(system)
    , m_system_context_(simulation, *this)
    , m_collections_(create_collections(m_system_context_, system))
    , m_phase_(std::move(phase))
    , m_scene_(std::move(scene))
{
  if (options().use_single_bus()) {
    const auto& buses = system.bus_array;
    if (!buses.empty()) {
      m_single_bus_id_.emplace(buses.front().uid);
    }
  }

  create_lp(flat_opts);
}

void SystemLP::write_out() const
{
  OutputContext oc(system_context(), linear_interface());

  visit_elements(collections(),
                 [&oc](const auto& e) { return e.add_to_output(oc); });

  oc.write();
}

void SystemLP::write_lp(const std::string& filename) const
{
  const auto fname = as_label(filename, phase().index(), scene().index());

  linear_interface().write_lp(fname);
}

std::expected<int, Error> SystemLP::resolve(const SolverOptions& solver_options)
{
  return linear_interface().resolve(solver_options);
}

}  // namespace gtopt
