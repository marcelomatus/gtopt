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
#include "gtopt/scenario.hpp"
#include "gtopt/simulation_lp.hpp"
#include "gtopt/system_context.hpp"
#include "gtopt/utils.hpp"

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
  // Reserve space for the output vector
  std::vector<Out> output;
  output.reserve(input.size());

  // Use transform algorithm instead of manual loop
  std::transform(input.begin(),
                 input.end(),
                 std::back_inserter(output),
                 [&](Inp element)
                 { return Out {input_context, std::move(element)}; });

  return Collection<Out> {std::move(output)};
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
                         const ScenarioIndex& scenario_index,
                         const StageIndex& stage_index,
                         LinearProblem& lp)
{
  const bool use_single_bus = system_context.options().use_single_bus();

  auto visitor = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, BusLP>) {
      return !use_single_bus || system_context.is_single_bus(e.id())
          ? e.add_to_lp(system_context, scenario_index, stage_index, lp)
          : true;
    } else if constexpr (std::is_same_v<T, LineLP>) {
      return !use_single_bus
          ? e.add_to_lp(system_context, scenario_index, stage_index, lp)
          : true;
    } else {
      return e.add_to_lp(system_context, scenario_index, stage_index, lp);
    }
  };

  visit_elements(collections, visitor);
}

constexpr auto create_linear_interfaces(auto& collections,
                                        auto& system_context,
                                        const auto& system,
                                        const auto& simulation,
                                        const auto& flat_opts)
{
  std::vector<LinearInterface> linear_interfaces;
  const auto& phases = simulation.phases();
  linear_interfaces.reserve(phases.size());

  for (const auto& phase : phases) {
    LinearProblem lp(system.name);

    // Process all active stages in phase
    for (auto&& [stage_index, stage] :
         enumerate_active<StageIndex>(phase.stages()))
    {
      // Process all active scenarios in simulation
      for (auto&& [scenario_index, scenario] :
           enumerate_active<ScenarioIndex>(simulation.scenarios()))
      {
        add_to_lp(collections, system_context, scenario_index, stage_index, lp);
      }
    }

    // Convert and store the flattened LP representation
    linear_interfaces.emplace_back(lp.to_flat(flat_opts));
  }

  return linear_interfaces;
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

SystemLP::SystemLP(const System& system,
                   SimulationLP& simulation,
                   PhaseIndex phase_index,
                   const FlatOptions& flat_opts)
    : m_system_(system)
    , m_system_context_(simulation, *this)
    , m_collections_(create_collections(m_system_context_, system))
    , m_linear_interfaces_(create_linear_interfaces(
          m_collections_, m_system_context_, system, simulation, flat_opts))
    , m_phase_index_(phase_index)
{
}

void SystemLP::write_out() const
{
  for (auto&& li : m_linear_interfaces_) {
    OutputContext oc(m_system_context_, li);

    visit_elements(m_collections_,
                   [&oc](const auto& e) { return e.add_to_output(oc); });

    oc.write();
  }
}

void SystemLP::write_lp(const std::string& filename) const
{
  for (auto&& [index, li] : enumerate(m_linear_interfaces_)) {
    const auto fname = fmt::format("{}_{}", filename, index);
    li.write_lp(fname);
  }
}

/**
 * @brief Resolves all linear programming interfaces with given solver
 * options
 *
 * Attempts to solve each linear programming problem in the system using the
 * provided solver options. Returns true only if all problems were solved
 * successfully.
 *
 * @param solver_options Configuration options for the LP solver
 * @return true if all LP problems were solved successfully
 * @return false if any LP problem failed to solve
 */
bool SystemLP::resolve(const SolverOptions& solver_options)
{
  return ranges::all_of(m_linear_interfaces_,
                        [&solver_options](auto& interface)
                        { return interface.resolve(solver_options); });
}

}  // namespace gtopt
