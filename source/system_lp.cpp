/**
 * @file      system_lp.cpp
 * @brief     Implementation of SystemLP class for power system LP formulation
 * @date      Tue Apr  8 01:20:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the SystemLP class, which is responsible for creating
 * and managing the linear programming formulation of power system optimization
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
#include <range/v3/view/iota.hpp>  // for ranges::views::iota#include <gtopt/input_context.hpp>
#include <spdlog/spdlog.h>

#include "gtopt/options_lp.hpp"
#include "gtopt/scenario.hpp"
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

}  // namespace

namespace gtopt
{

inline void SystemLP::initialize_collections(
    const SystemContext& system_context)
{
  InputContext input_context(system_context);

  std::get<Collection<BusLP>>(m_collections_) =
      make_collection<BusLP>(input_context, system().bus_array);
  std::get<Collection<DemandLP>>(m_collections_) =
      make_collection<DemandLP>(input_context, system().demand_array);
  std::get<Collection<GeneratorLP>>(m_collections_) =
      make_collection<GeneratorLP>(input_context, system().generator_array);
  std::get<Collection<LineLP>>(m_collections_) =
      make_collection<LineLP>(input_context, system().line_array);
  std::get<Collection<GeneratorProfileLP>>(m_collections_) =
      make_collection<GeneratorProfileLP>(input_context,
                                          system().generator_profile_array);
  std::get<Collection<DemandProfileLP>>(m_collections_) =
      make_collection<DemandProfileLP>(input_context,
                                       system().demand_profile_array);
  std::get<Collection<BatteryLP>>(m_collections_) =
      make_collection<BatteryLP>(input_context, system().battery_array);
  std::get<Collection<ConverterLP>>(m_collections_) =
      make_collection<ConverterLP>(input_context, system().converter_array);
  std::get<Collection<ReserveZoneLP>>(m_collections_) =
      make_collection<ReserveZoneLP>(input_context,
                                     system().reserve_zone_array);
  std::get<Collection<ReserveProvisionLP>>(m_collections_) =
      make_collection<ReserveProvisionLP>(input_context,
                                          system().reserve_provision_array);
}

#ifdef GTOPT_EXTRA
void SystemLP::initialize_extra_collections()
{
  InputContext input_context(options(), *this);

  std::get<Collection<JunctionLP>>(m_collections_) =
      make_collection<JunctionLP>(input_context, m_system_.junctions);
  std::get<Collection<WaterwayLP>>(m_collections_) =
      make_collection<WaterwayLP>(input_context, m_system_.waterways);
  std::get<Collection<InflowLP>>(m_collections_) =
      make_collection<InflowLP>(input_context, m_system_.inflows);
  std::get<Collection<OutflowLP>>(m_collections_) =
      make_collection<OutflowLP>(input_context, m_system_.outflows);
  std::get<Collection<ReservoirLP>>(m_collections_) =
      make_collection<ReservoirLP>(input_context, m_system_.reservoirs);
  std::get<Collection<FiltrationLP>>(m_collections_) =
      make_collection<FiltrationLP>(input_context, m_system_.filtrations);
  std::get<Collection<TurbineLP>>(m_collections_) =
      make_collection<TurbineLP>(input_context, m_system_.turbines);
  std::get<Collection<EmissionZoneLP>>(m_collections_) =
      make_collection<EmissionZoneLP>(input_context, m_system_.emission_zones);
  std::get<Collection<GeneratorEmissionLP>>(m_collections_) =
      make_collection<GeneratorEmissionLP>(input_context,
                                           m_system_.generator_emissions);
  std::get<Collection<DemandEmissionLP>>(m_collections_) =
      make_collection<DemandEmissionLP>(input_context,
                                        m_system_.demand_emissions);
}
#endif

SystemLP::SystemLP(System system, const SimulationLP& simulation)
    : m_system_(std::move(system))
    , m_system_context_(simulation, *this)
{
  initialize_collections(m_system_context_);
}

void SystemLP::add_to_lp(const SystemContext& system_context,
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

  visit_elements(m_collections_, visitor);
}

void SystemLP::create_linear_problems(const SimulationLP& simulation,
                                      const SceneLP& scene,
                                      const std::vector<PhaseLP>& phases)
{
  std::vector<LinearInterface> linear_problems;
  linear_problems.reserve(phases.size());

  SystemContext system_context(simulation, *this);
  for (const auto scenario_index :
       ranges::views::iota(0, scene.count_scenario())
           | ranges::views::transform([](auto i) { return ScenarioIndex {i}; }))
  {
    LinearProblem lp;
    for (const auto& phase : phases) {
      for (const auto stage_index : ranges::views::iota(0, phase.count_stage())
               | ranges::views::transform([](auto i)
                                          { return StageIndex {i}; }))
      {
        add_to_lp(system_context, scenario_index, stage_index, lp);
      }
    }

    linear_problems.emplace_back(lp.to_flat());
  }

  m_linear_problems_ = std::move(linear_problems);
}

constexpr void SystemLP::write_out(const SystemContext& system_context,
                                   const LinearInterface& li) const
{
  OutputContext oc(system_context, li);

  visit_elements(m_collections_,
                 [&oc](const auto& e) { return e.add_to_output(oc); });

  oc.write();
}

void SystemLP::write_out() const
{
  for (auto&& li : m_linear_problems_) {
    write_out(m_system_context_, li);
  }
}

bool SystemLP::solve_linear_problems()
{
  for (auto&& lp : m_linear_problems_) {
    auto result = lp.resolve();
    if (!result) {
      return result;
    }
  }
  return true;
}

}  // namespace gtopt
