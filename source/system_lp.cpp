#include <algorithm>
#include <stdexcept>

#include <gtopt/system_lp.hpp>
#include <range/v3/all.hpp>
#include <range/v3/view/iota.hpp>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

template<typename Collections, typename Op>
constexpr size_t visit_elements(Collections&& collections, Op op)
{
  namespace rgs = std::ranges;

  size_t count = 0;
  std::apply(
      [&](auto&&... c)
      {
        ((rgs::for_each(std::forward<decltype(c)>(c).elements(),
                        [&](auto&& e)
                        {
                          if (op(e)) [[likely]] {
                            ++count;
                          }
                        })),
         ...);
      },
      collections);
  return count;
}

template<typename Collections, typename SContext, typename Op>
constexpr void system_apply(Collections& collections, SContext& sc, Op op)
{
  size_t count = 0;
  for (auto&& [scenery_index, scenery] :
       enumerate_active<SceneryIndex>(sc.system().sceneries()))
  {
    sc.set_scenery(scenery_index, scenery);

    const auto use_single_bus = sc.options().use_single_bus();

    for (auto&& [stage_index, stage] :
         enumerate_active<StageIndex>(sc.system().stages()))
    {
      sc.set_stage(stage_index, stage);

      auto overload = Overload {
          [&](BusLP& e)
          {
            return !use_single_bus || sc.is_single_bus(e.id()) ? op(e) : true;
          },
          [&](LineLP& e) { return !use_single_bus ? op(e) : true; },
          [&](auto& e) { return op(e); }};

      const auto napply = visit_elements(collections, overload);
      count += napply;
    }
  }

  SPDLOG_TRACE("successfully visited and applied {} elements", count);
}

template<typename Out, typename Inp, typename InputContext>
constexpr auto make_collection(InputContext& ic, std::vector<Inp>& input)
    -> Collection<Out>
{
  std::vector<Out> output;
  output.reserve(input.size());
  for (auto&& e : input) {
    output.emplace_back(Out {ic, std::move(e)});
  }

  return Collection<Out> {std::move(output)};
}

template<typename Out, typename Inp, typename InputContext>
constexpr auto make_collection(InputContext& ic,
                               std::optional<std::vector<Inp>>& input)
    -> Collection<Out>
{
  if (input.has_value()) [[likely]] {
    return make_collection<Out>(ic, input.value());
  }
  return Collection<Out> {};
}

constexpr bool needs_theta_ref(const auto& buses, const auto& options)
{
  return buses.size() > 1 && !options.use_single_bus()
      && options.use_kirchhoff()
      && std::none_of(buses.begin(),
                      buses.end(),
                      [](const auto& b) { return b.reference_theta; })
      && std::any_of(
             buses.begin(),
             buses.end(),
             [&](const auto& b)
             { return b.needs_kirchhoff(options.kirchhoff_threshold()); });
}

}  // namespace

namespace gtopt
{

SystemLP::SystemLP(System&& psystem)
    : m_system_(std::move(psystem))
    , m_options_(std::move(m_system_.options))
    , m_blocks_(  //
          m_system_.block_array | ranges::views::move
          | ranges::views::transform(
              [](auto&& s) { return BlockLP {std::forward<decltype(s)>(s)}; })
          | ranges::to<std::vector>())
    , m_stages_(  //
          m_system_.stage_array | ranges::views::move
          | ranges::views::transform(
              [this](auto&& s)
              {
                return StageLP {std::forward<decltype(s)>(s),
                                m_blocks_,
                                m_options_.annual_discount_rate()};
              })
          | ranges::to<std::vector>())
    , m_sceneries_(  //
          m_system_.scenery_array | ranges::views::move
          | ranges::views::transform(
              [this](auto&& s)
              { return SceneryLP {std::forward<decltype(s)>(s), m_stages_}; })
          | ranges::to<std::vector>())
    , sc(*this)
    , ic(sc)
{
  //
  // check empty block, stage and sceneries, and the number of blocks in stages
  //
  if (m_blocks_.empty() || m_stages_.empty() || m_sceneries_.empty()) {
    const auto* const msg =
        "you must provide at least one block, stage and scenery";
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  const auto nblocks = std::accumulate(m_stages_.begin(),  // NOLINT
                                       m_stages_.end(),
                                       0U,
                                       [](size_t a, const auto& s)
                                       { return a + s.blocks().size(); });
  if (nblocks != m_blocks_.size()) {
    const auto* const msg =
        " blocks in stages doen't match the real number ob blocks founded";
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  //
  // check if one bus with theta_ref set is needed and present
  //
  if (needs_theta_ref(m_system_.bus_array, m_options_)) {
    auto& bus = m_system_.bus_array.front();
    bus.reference_theta = 0;
    SPDLOG_WARN(
        "you need to set reference_theta=0 for at lest one bus, now using {} "
        "as reference bus",
        bus.name);
  }

  std::get<Collection<BusLP>>(m_collections_) =
      make_collection<BusLP>(ic, m_system_.bus_array);

  std::get<Collection<DemandLP>>(m_collections_) =
      make_collection<DemandLP>(ic, m_system_.demand_array);

  std::get<Collection<GeneratorLP>>(m_collections_) =
      make_collection<GeneratorLP>(ic, m_system_.generator_array);

  std::get<Collection<LineLP>>(m_collections_) =
      make_collection<LineLP>(ic, m_system_.line_array);

#ifdef GTOPT_EXTRA
  std::get<Collection<GeneratorProfileLP>>(m_collections_) =
      make_collection<GeneratorProfileLP>(
          ic, std::move(m_system_.generator_profiles));

  std::get<Collection<DemandProfileLP>>(m_collections_) =
      make_collection<DemandProfileLP>(ic,
                                       std::move(m_system_.demand_profiles));

  std::get<Collection<BatteryLP>>(m_collections_) =
      make_collection<BatteryLP>(ic, std::move(m_system_.batteries));

  std::get<Collection<ConverterLP>>(m_collections_) =
      make_collection<ConverterLP>(ic, std::move(m_system_.converters));

  std::get<Collection<JunctionLP>>(m_collections_) =
      make_collection<JunctionLP>(ic, std::move(m_system_.junctions));

  std::get<Collection<WaterwayLP>>(m_collections_) =
      make_collection<WaterwayLP>(ic, std::move(m_system_.waterways));

  std::get<Collection<InflowLP>>(m_collections_) =
      make_collection<InflowLP>(ic, std::move(m_system_.inflows));

  std::get<Collection<OutflowLP>>(m_collections_) =
      make_collection<OutflowLP>(ic, std::move(m_system_.outflows));

  std::get<Collection<ReservoirLP>>(m_collections_) =
      make_collection<ReservoirLP>(ic, std::move(m_system_.reservoirs));

  std::get<Collection<FiltrationLP>>(m_collections_) =
      make_collection<FiltrationLP>(ic, std::move(m_system_.filtrations));

  std::get<Collection<TurbineLP>>(m_collections_) =
      make_collection<TurbineLP>(ic, std::move(m_system_.turbines));

  std::get<Collection<ReserveZoneLP>>(m_collections_) =
      make_collection<ReserveZoneLP>(ic, std::move(m_system_.reserve_zones));

  std::get<Collection<ReserveProvisionLP>>(m_collections_) =
      make_collection<ReserveProvisionLP>(
          ic, std::move(m_system_.reserve_provisions));

  std::get<Collection<EmissionZoneLP>>(m_collections_) =
      make_collection<EmissionZoneLP>(ic, std::move(m_system_.emission_zones));

  std::get<Collection<GeneratorEmissionLP>>(m_collections_) =
      make_collection<GeneratorEmissionLP>(
          ic, std::move(m_system_.generator_emissions));

  std::get<Collection<DemandEmissionLP>>(m_collections_) =
      make_collection<DemandEmissionLP>(ic,
                                        std::move(m_system_.demand_emissions));

#endif
}

void SystemLP::add_to_lp(LinearProblem& lp)
{
  system_apply(
      m_collections_, sc, [this, &lp](auto& e) { return e.add_to_lp(sc, lp); });
}

void SystemLP::write_out(const LinearInterface& li) const
{
  OutputContext oc(sc, li);

  visit_elements(m_collections_, [&](auto&& e) { return e.add_to_output(oc); });

  oc.write();
}

}  // namespace gtopt
