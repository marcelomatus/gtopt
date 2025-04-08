/**
 * @file      system_lp.cpp
 * @brief     Header of
 * @date      Tue Apr  8 01:20:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <algorithm>

#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

template<typename Collections, typename SContext, typename Op>
constexpr void system_apply(Collections& collections, SContext& sc, Op op)
{
  std::size_t count = 0;
  const auto& system = sc.system();
  const bool use_single_bus = sc.options().use_single_bus();

  // Create overload pattern once outside the loops
  auto overload = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, BusLP>) {
      return !use_single_bus || sc.is_single_bus(e.id()) ? op(e) : true;
    } else if constexpr (std::is_same_v<T, LineLP>) {
      return !use_single_bus ? op(e) : true;
    } else {
      return op(e);
    }
  };

  // Iterate through active sceneries
  for (auto&& [scenery_index, scenery] :
       enumerate_active<SceneryIndex>(system.sceneries()))
  {
    sc.set_scenery(scenery_index, scenery);

    // Iterate through active stages
    for (auto&& [stage_index, stage] :
         enumerate_active<StageIndex>(system.stages()))
    {
      sc.set_stage(stage_index, stage);

      // Process elements
      const auto napply = visit_elements(collections, overload);
      count += napply;
    }
  }

  SPDLOG_TRACE("Successfully visited and applied {} elements", count);
}

template<typename Out, typename Inp, typename InputContext>
constexpr auto make_collection(InputContext& ic, std::vector<Inp>& input)
    -> Collection<Out>
{
  // Reserve space for the output vector
  std::vector<Out> output;
  output.reserve(input.size());

  // Use transform algorithm instead of manual loop
  std::transform(std::make_move_iterator(input.begin()),
                 std::make_move_iterator(input.end()),
                 std::back_inserter(output),
                 [&ic](Inp&& element) { return Out {ic, std::move(element)}; });

  return Collection<Out> {std::move(output)};
}

template<typename Out, typename Inp, typename InputContext>
constexpr auto make_collection(InputContext& ic,
                               const std::optional<std::vector<Inp>>& input)
    -> Collection<Out>
{
  if (input) [[likely]] {
    return make_collection<Out>(ic, *input);
  }
  return Collection<Out> {};
}

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

inline std::vector<SceneryLP> SystemLP::create_scenery_array()
{
  return m_system_.scenery_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return SceneryLP {std::forward<decltype(s)>(s), m_stage_array_};
             })
      | ranges::to<std::vector>();
}

inline std::vector<PeriodLP> SystemLP::create_period_array()
{
  return m_system_.period_array | ranges::views::move
      | ranges::views::transform(
             [this](auto&& s)
             {
               return PeriodLP {std::forward<decltype(s)>(s), m_stage_array_};
             })
      | ranges::to<std::vector>();
}

inline void SystemLP::validate_system_components()
{
  if (m_block_array_.empty() || m_stage_array_.empty()
      || m_scenery_array_.empty())
  {
    throw std::runtime_error(
        "System must contain at least one block, stage, and scenery");
  }

  const auto nblocks = std::accumulate(m_stage_array_.begin(),  // NOLINT
                                       m_stage_array_.end(),
                                       0U,
                                       [](size_t a, const auto& s)
                                       { return a + s.blocks().size(); });

  if (nblocks != m_block_array_.size()) {
    throw std::runtime_error(
        "Number of blocks in stages doesn't match the total number of blocks");
  }
}

inline void SystemLP::setup_reference_bus()
{
  if (needs_ref_theta(m_system_.bus_array, m_options_)) {
    auto& bus = m_system_.bus_array.front();
    bus.reference_theta = 0;
    SPDLOG_WARN("Setting bus '{}' as reference bus (reference_theta=0)",
                bus.name);
  }
}

inline void SystemLP::initialize_collections()
{
  std::get<Collection<BusLP>>(m_collections_) =
      make_collection<BusLP>(ic, m_system_.bus_array);
  std::get<Collection<DemandLP>>(m_collections_) =
      make_collection<DemandLP>(ic, m_system_.demand_array);
  std::get<Collection<GeneratorLP>>(m_collections_) =
      make_collection<GeneratorLP>(ic, m_system_.generator_array);
  std::get<Collection<LineLP>>(m_collections_) =
      make_collection<LineLP>(ic, m_system_.line_array);
  std::get<Collection<GeneratorProfileLP>>(m_collections_) =
      make_collection<GeneratorProfileLP>(ic,
                                          m_system_.generator_profile_array);
  std::get<Collection<DemandProfileLP>>(m_collections_) =
      make_collection<DemandProfileLP>(ic, m_system_.demand_profile_array);
  std::get<Collection<BatteryLP>>(m_collections_) =
      make_collection<BatteryLP>(ic, m_system_.battery_array);
  std::get<Collection<ConverterLP>>(m_collections_) =
      make_collection<ConverterLP>(ic, m_system_.converter_array);
  std::get<Collection<ReserveZoneLP>>(m_collections_) =
      make_collection<ReserveZoneLP>(ic, m_system_.reserve_zone_array);
  std::get<Collection<ReserveProvisionLP>>(m_collections_) =
      make_collection<ReserveProvisionLP>(ic,
                                          m_system_.reserve_provision_array);
}

#ifdef GTOPT_EXTRA
void SystemLP::initializeExtraCollections()
{
  std::get<Collection<JunctionLP>>(m_collections_) =
      make_collection<JunctionLP>(ic, m_system_.junctions);
  std::get<Collection<WaterwayLP>>(m_collections_) =
      make_collection<WaterwayLP>(ic, m_system_.waterways);
  std::get<Collection<InflowLP>>(m_collections_) =
      make_collection<InflowLP>(ic, m_system_.inflows);
  std::get<Collection<OutflowLP>>(m_collections_) =
      make_collection<OutflowLP>(ic, m_system_.outflows);
  std::get<Collection<ReservoirLP>>(m_collections_) =
      make_collection<ReservoirLP>(ic, m_system_.reservoirs);
  std::get<Collection<FiltrationLP>>(m_collections_) =
      make_collection<FiltrationLP>(ic, m_system_.filtrations);
  std::get<Collection<TurbineLP>>(m_collections_) =
      make_collection<TurbineLP>(ic, m_system_.turbines);
  std::get<Collection<EmissionZoneLP>>(m_collections_) =
      make_collection<EmissionZoneLP>(ic, m_system_.emission_zones);
  std::get<Collection<GeneratorEmissionLP>>(m_collections_) =
      make_collection<GeneratorEmissionLP>(ic, m_system_.generator_emissions);
  std::get<Collection<DemandEmissionLP>>(m_collections_) =
      make_collection<DemandEmissionLP>(ic, m_system_.demand_emissions);
}
#endif

SystemLP::SystemLP(System psystem)
    : m_system_(std::move(psystem))
    , m_options_(std::move(m_system_.options))
    , m_block_array_(create_block_array())
    , m_stage_array_(create_stage_array())
    , m_scenery_array_(create_scenery_array())
    , m_period_array_(create_period_array())
    , sc(*this)
    , ic(sc)
{
  validate_system_components();
  setup_reference_bus();
  initialize_collections();
}

void SystemLP::add_to_lp(LinearProblem& lp)
{
  system_apply(m_collections_,
               this->sc,
               [this, &lp](auto& e) { return e.add_to_lp(this->sc, lp); });
}

void SystemLP::write_out(const LinearInterface& li) const
{
  OutputContext oc(this->sc, li);

  visit_elements(m_collections_,
                 [&oc](const auto& e) { return e.add_to_output(oc); });

  oc.write();
}

}  // namespace gtopt
