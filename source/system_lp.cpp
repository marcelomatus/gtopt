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

#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

/**
 * @brief Applies an operation to all elements in collections across active
 * scenarios and stages
 *
 * This function iterates through all active scenarios and stages in the system
 * and applies the given operation to each element in the collections. It
 * handles special cases for buses and lines when single-bus mode is enabled.
 *
 * @tparam Collections Type of the collections container
 * @tparam SContext Type of the system context
 * @tparam Op Type of the operation to apply
 * @param collections Collections of system elements
 * @param system_context System context containing current state
 * @param op Operation to apply to each element
 *
 * The operation should have the signature:
 * `bool op(ElementType& element)`
 * and should return true if the operation succeeded, false otherwise.
 */
template<typename Collections, typename SContext, typename Op>
constexpr void system_apply(Collections& collections,
                            SContext& system_context,
                            Op op)
{
  std::size_t count = 0;
  const auto& system = system_context.system();
  const bool use_single_bus = system_context.options().use_single_bus();

  // Create overload pattern once outside the loops
  auto overload = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, BusLP>) {
      return !use_single_bus || system_context.is_single_bus(e.id()) ? op(e)
                                                                     : true;
    } else if constexpr (std::is_same_v<T, LineLP>) {
      return !use_single_bus ? op(e) : true;
    } else {
      return op(e);
    }
  };

  // Iterate through active scenarios
  for (auto&& [scenario_index, scenario] :
       enumerate_active<ScenarioIndex>(system.scenarios()))
  {
    system_context.set_scenario(scenario_index, scenario);

    // Iterate through active stages
    for (auto&& [stage_index, stage] :
         enumerate_active<StageIndex>(system.stages()))
    {
      system_context.set_stage(stage_index, stage);

      // Process elements
      const auto napply = visit_elements(collections, overload);
      count += napply;
    }
  }

  SPDLOG_TRACE("Successfully visited and applied {} elements", count);
}

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
                               std::vector<Inp>& input) -> Collection<Out>
{
  // Reserve space for the output vector
  std::vector<Out> output;
  output.reserve(input.size());

  // Use transform algorithm instead of manual loop
  std::transform(std::make_move_iterator(input.begin()),
                 std::make_move_iterator(input.end()),
                 std::back_inserter(output),
                 [&input_context](Inp&& element)
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
  const bool has_reference_bus =
      std::any_of(buses.begin(),
                  buses.end(),
                  [](const auto& bus) { return bus.reference_theta; });

  if (has_reference_bus) {
    return false;
  }

  // Check if any bus needs Kirchhoff according to the threshold
  const auto kirchhoff_threshold = options.kirchhoff_threshold();
  return std::any_of(buses.begin(),
                     buses.end(),
                     [kirchhoff_threshold](const auto& bus)
                     { return bus.needs_kirchhoff(kirchhoff_threshold); });
}

}  // namespace

namespace gtopt
{

inline std::vector<BlockLP> SystemLP::create_block_array()
{
  return m_system_.block_array | ranges::views::move
      | ranges::views::transform(
             [](auto&& s) { return BlockLP {std::forward<decltype(s)>(s)}; })
      | ranges::to<std::vector>();
}

inline std::vector<StageLP> SystemLP::create_stage_array()
{
  return m_system_.stage_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return StageLP {std::forward<decltype(s)>(s),
                               m_block_array_,
                               m_options_.annual_discount_rate()};
             })
      | ranges::to<std::vector>();
}

inline std::vector<ScenarioLP> SystemLP::create_scenario_array()
{
  return m_system_.scenario_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return ScenarioLP {std::forward<decltype(s)>(s), m_stage_array_};
             })
      | ranges::to<std::vector>();
}

inline std::vector<PhaseLP> SystemLP::create_phase_array()
{
  if (m_system_.phase_array.empty()) {
    m_system_.phase_array = {{.uid = 0,
                              .name = "",
                              .active = true,
                              .first_stage = 0,
                              .count_stage = m_stage_array_.size()}};
  }

  return m_system_.phase_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             { return PhaseLP {std::forward<decltype(s)>(s), m_stage_array_}; })
      | ranges::to<std::vector>();
}

inline std::vector<SceneLP> SystemLP::create_scene_array()
{
  if (m_system_.scene_array.empty()) {
    m_system_.scene_array = {{.uid = 0,
                              .name = "",
                              .active = true,
                              .first_scenario = 0,
                              .count_scenario = m_scenario_array_.size()}};
  }

  return m_system_.scene_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return SceneLP {std::forward<decltype(s)>(s), m_scenario_array_};
             })
      | ranges::to<std::vector>();
}

inline void SystemLP::validate_system_components()
{
  if (m_block_array_.empty() || m_stage_array_.empty()
      || m_scenario_array_.empty())
  {
    throw std::runtime_error(
        "System must contain at least one block, stage, and scenario");
  }

  const auto nblocks = std::accumulate(m_stage_array_.begin(),  // NOLINT
                                       m_stage_array_.end(),
                                       0U,
                                       [](size_t a, const auto& s)
                                       { return a + s.blocks().size(); });

  if (nblocks != m_block_array_.size()) {
    throw std::runtime_error(
        "Number of blocks in stages doesn't match the total number of "
        "blocks");
  }
}

inline void SystemLP::setup_reference_bus()
{
  if (needs_ref_theta(m_system_.bus_array, m_options_)) {
    auto& bus = m_system_.bus_array.front();
    bus.reference_theta = 0;
    const auto msg = std::format(
        "Setting bus '{}' as reference bus (reference_theta=0)", bus.name);
    SPDLOG_WARN(msg);
  }
}

inline void SystemLP::initialize_collections()
{
  std::get<Collection<BusLP>>(m_collections_) =
      make_collection<BusLP>(input_context, m_system_.bus_array);
  std::get<Collection<DemandLP>>(m_collections_) =
      make_collection<DemandLP>(input_context, m_system_.demand_array);
  std::get<Collection<GeneratorLP>>(m_collections_) =
      make_collection<GeneratorLP>(input_context, m_system_.generator_array);
  std::get<Collection<LineLP>>(m_collections_) =
      make_collection<LineLP>(input_context, m_system_.line_array);
  std::get<Collection<GeneratorProfileLP>>(m_collections_) =
      make_collection<GeneratorProfileLP>(input_context,
                                          m_system_.generator_profile_array);
  std::get<Collection<DemandProfileLP>>(m_collections_) =
      make_collection<DemandProfileLP>(input_context,
                                       m_system_.demand_profile_array);
  std::get<Collection<BatteryLP>>(m_collections_) =
      make_collection<BatteryLP>(input_context, m_system_.battery_array);
  std::get<Collection<ConverterLP>>(m_collections_) =
      make_collection<ConverterLP>(input_context, m_system_.converter_array);
  std::get<Collection<ReserveZoneLP>>(m_collections_) =
      make_collection<ReserveZoneLP>(input_context,
                                     m_system_.reserve_zone_array);
  std::get<Collection<ReserveProvisionLP>>(m_collections_) =
      make_collection<ReserveProvisionLP>(input_context,
                                          m_system_.reserve_provision_array);
}

#ifdef GTOPT_EXTRA
void SystemLP::initialize_extra_collections()
{
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

SystemLP::SystemLP(System psystem)
    : m_system_(std::move(psystem))
    , m_options_(std::move(m_system_.options))
    , m_block_array_(create_block_array())
    , m_stage_array_(create_stage_array())
    , m_scenario_array_(create_scenario_array())
    , m_phase_array_(create_phase_array())
    , m_scene_array_(create_scene_array())
    , system_context(*this)
    , input_context(system_context)
{
  validate_system_components();
  setup_reference_bus();
  initialize_collections();

#ifdef GTOPT_EXTRA
  initialize_extra_collections();
#endif
}

void SystemLP::add_to_lp(LinearProblem& lp,
                         const StageIndex& stage_index,
                         const StageLP& stage,
                         const ScenarioIndex& scenario_index,
                         const ScenarioLP& scenario)
{
  system_context.set_scenario(scenario_index, scenario);
  system_context.set_stage(stage_index, stage);

  const bool use_single_bus = system_context.options().use_single_bus();

  auto visitor = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, BusLP>) {
      return !use_single_bus || system_context.is_single_bus(e.id())
          ? e.add_to_lp(system_context, lp)
          : true;
    } else if constexpr (std::is_same_v<T, LineLP>) {
      return !use_single_bus ? e.add_to_lp(system_context, lp) : true;
    } else {
      return e.add_to_lp(system_context, lp);
    }
  };

  visit_elements(m_collections_, visitor);
}

void SystemLP::add_to_lp(LinearProblem& lp)
{
  // Iterate through active scenarios
  for (auto&& [scenario_index, scenario] :
       enumerate_active<ScenarioIndex>(scenarios()))
  {
    // Iterate through active stages
    for (auto&& [stage_index, stage] : enumerate_active<StageIndex>(stages())) {
      // Process elements
      add_to_lp(lp, stage_index, stage, scenario_index, scenario);
    }
  }
}

void SystemLP::write_out(const LinearInterface& li) const
{
  OutputContext oc(this->system_context, li);

  visit_elements(m_collections_,
                 [&oc](const auto& e) { return e.add_to_output(oc); });

  oc.write();
}

}  // namespace gtopt
