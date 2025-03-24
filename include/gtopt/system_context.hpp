/**
 * @file      system_context.hpp
 * @brief     Header of
 * @date      Sun Mar 23 21:54:14 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <utility>

#include <gtopt/basic_types.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/scenery_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_options_lp.hpp>

namespace gtopt
{

class Bus;
class BusLP;

template<typename SystemContext, typename Element>
struct ElementTraits
{
  constexpr static auto&& get_elements(SystemContext& sc)
  {
    return sc.system().template elements<Element>();
  }

  template<template<typename> class Id>
  constexpr static auto get_element_index(SystemContext& sc,
                                          const Id<Element>& id)
  {
    return sc.system().element_index(id);
  }

  template<template<typename> class Id>
  constexpr static auto&& get_element(SystemContext& sc, const Id<Element>& id)
  {
    return sc.system().element(id);
  }

  template<typename ET>
  constexpr static auto push_back(SystemContext& sc, ET&& e)
  {
    return sc.system().template push_back<Element>(std::forward<ET>(e));
  }
};

template<typename SystemContext>
struct ElementTraits<SystemContext, BusLP>
{
  constexpr static auto&& get_elements(SystemContext& sc)
  {
    return sc.system().template elements<BusLP>();
  }

  constexpr static auto get_element_index(SystemContext& sc,
                                          const ObjectSingleId<BusLP>& id)
  {
    return sc.get_bus_index(id);
  }

  constexpr static auto&& get_element(SystemContext& sc,
                                      const ObjectSingleId<BusLP>& id)
  {
    return sc.get_bus(id);
  }

  template<typename BusType>
  constexpr static auto&& get_element(SystemContext& sc,
                                      const ElementIndex<BusType>& id)
  {
    return sc.system().element(id);
  }

  template<typename ET>
  constexpr static auto push_back(SystemContext& sc, ET&& e)
  {
    return sc.system().template push_back<BusLP>(std::forward<ET>(e));
  }
};

template<typename Element, typename SystemContext>
constexpr auto&& get_elements(SystemContext& sc)
{
  return ElementTraits<SystemContext, Element>::get_elements(sc);
}

template<typename Element, typename SystemContext, template<typename> class Id>
constexpr auto get_element_index(SystemContext& sc, const Id<Element>& id)
{
  return ElementTraits<SystemContext, Element>::get_element_index(sc, id);
}

template<typename Element, typename SystemContext, template<typename> class Id>
constexpr auto&& get_element(SystemContext& sc, const Id<Element>& id)
{
  return ElementTraits<SystemContext, Element>::get_element(sc, id);
}

template<typename Element, typename SystemContext>
constexpr auto push_back(SystemContext& sc, Element&& e)
{
  return ElementTraits<SystemContext, Element>::push_back(
      sc, std::forward<Element>(e));
}

using STBUids =
    std::tuple<std::vector<Uid>, std::vector<Uid>, std::vector<Uid>>;

using STUids = std::tuple<std::vector<Uid>, std::vector<Uid>>;
using TUids = std::vector<Uid>;

class SystemLP;
class SystemContext
{
public:
  explicit SystemContext(SystemLP& psystem);

  [[nodiscard]] constexpr auto scenery_index() const
  {
    return m_scenery_index_;
  }

  [[nodiscard]] constexpr auto scenery_uid() const { return m_scenery_uid_; }

  [[nodiscard]] constexpr auto scenery_probability_factor() const
  {
    return m_scenery_probability_factor_;
  }

  [[nodiscard]] constexpr auto stage_index() const { return m_stage_index_; }
  [[nodiscard]] constexpr auto stage_uid() const { return m_stage_uid_; }
  [[nodiscard]] constexpr auto stage_discount_factor() const
  {
    return m_stage_discount_factor_;
  }

  [[nodiscard]] constexpr auto stage_duration(const OptStageIndex& stage_index,
                                              double prev_duration = 0) const
  {
    return stage_index ? stages().at(stage_index.value()).duration()
                       : prev_duration;
  }

  [[nodiscard]] constexpr auto stage_duration() const
  {
    return m_stage_duration_;
  }
  [[nodiscard]] constexpr auto&& stage_blocks() const
  {
    return m_stage_blocks_;
  }
  [[nodiscard]] constexpr auto&& stage_block_indexes() const
  {
    return m_stage_block_indexes_;
  }

  [[nodiscard]] constexpr std::pair<const StageLP::BlockSpan&,
                                    const StageLP::BlockIndexSpan&>
  stage_blocks_and_indexes() const
  {
    return {m_stage_blocks_, m_stage_block_indexes_};
  }

  [[nodiscard]] const SystemOptionsLP& options() const;

  template<typename... Types>
  constexpr auto label(const Types&... var) const -> std::string
  {
    if (!options().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var...);
  }

  template<typename... Types>
    requires(sizeof...(Types) == 3)
  constexpr auto t_label(const Types&... var) const -> std::string
  {
    if (!options().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var..., stage_uid());
  }

  template<typename... Types>
    requires(sizeof...(Types) == 3)
  constexpr auto st_label(const Types&... var) const -> std::string
  {
    if (!options().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var..., scenery_uid(), stage_uid());
  }

  template<typename... Types>
    requires(sizeof...(Types) == 3)
  constexpr auto stb_label(const BlockLP& block, const Types&... var) const
      -> std::string
  {
    if (!options().use_lp_names()) [[likely]] {
      return {};
    }
    return gtopt::as_label(var..., scenery_uid(), stage_uid(), block.uid());
  }

  template<typename LossFactor>
  constexpr auto stage_lossfactor(const LossFactor& lfact) const
  {
    return options().use_line_losses()
        ? std::max(lfact.at(stage_index()).value_or(0.0), 0.0)
        : 0.0;
  }

  template<typename Reactance>
  constexpr auto stage_reactance(const Reactance& reactance) const
  {
    return options().use_kirchhoff() ? reactance.at(stage_index())
                                     : decltype(reactance.at(stage_index())) {};
  }

  template<typename FailCost>
  constexpr auto demand_fail_cost(const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage_index());
    return fc ? fc : options().demand_fail_cost();
  }

  template<typename FailCost>
  constexpr auto reserve_fail_cost(const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage_index());
    return fc ? fc : options().reserve_fail_cost();
  }

  [[nodiscard]] auto is_first_scenery() const
  {
    return scenery_index() == active_sceneries.front();
  }

  [[nodiscard]] auto is_last_scenery() const
  {
    return scenery_index() == active_sceneries.back();
  }

  [[nodiscard]] auto active_scenery_count() const
  {
    return active_sceneries.size();
  }
  [[nodiscard]] auto active_stage_count() const { return active_stages.size(); }
  [[nodiscard]] auto active_block_count() const { return active_blocks.size(); }

  [[nodiscard]] auto is_first_stage() const
  {
    return stage_index() == active_stages.front();
  }
  [[nodiscard]] auto is_last_stage() const
  {
    return stage_index() == active_stages.back();
  }

  [[nodiscard]] auto is_first() const
  {
    return is_first_scenery() && is_first_stage();
  }

  [[nodiscard]] double block_cost(const BlockLP& block, double cost) const;
  [[nodiscard]] auto block_cost_factors() const -> std::vector<double>;

  [[nodiscard]] double stage_cost(double cost) const;
  [[nodiscard]] auto stage_cost_factors() const -> std::vector<double>;

  [[nodiscard]] auto stb_active_uids() const -> STBUids;
  [[nodiscard]] auto st_active_uids() const -> STUids;
  [[nodiscard]] auto t_active_uids() const -> TUids;

  [[nodiscard]] auto stb_uids() const -> STBUids;
  [[nodiscard]] auto st_uids() const -> STUids;
  [[nodiscard]] auto t_uids() const -> TUids;

  template<typename Projection, typename Span = std::span<double>>
  constexpr auto flat(const GSTBIndexHolder& hstb,
                      Projection proj,
                      const Span& factor = {}) const
  {
    const auto size = active_scenery_count() * active_block_count();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    using proj_type = std::function<double(size_t)>;
    const auto proj2 = factor.empty()
        ? proj_type {proj}
        : proj_type {[&](auto index) { return proj(index) * factor[idx]; }};

    for (size_t count = 0; auto&& sindex : active_sceneries) {
      for (auto&& tindex : active_stages) {
        for (auto&& bindex : active_stage_blocks[tindex]) {
          auto&& stbiter = hstb.find({sindex, tindex, bindex});
          if (stbiter != hstb.end()) {
            values[idx] = proj2(stbiter->second);
            valid[idx] = true;
            ++count;

            need_values = true;
          }
          need_valids |= count != ++idx;
        }
      }
    }

    return std::make_pair(need_values ? values : std::vector<double> {},
                          need_valids ? valid : std::vector<bool> {});
  }

  template<typename Projection, typename Span = std::span<double>>
  constexpr auto flat(const STBIndexHolder& hstb,
                      Projection proj,
                      const Span& factor = {}) const
  {
    const auto size = active_scenery_count() * active_block_count();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    using proj_type = std::function<double(size_t)>;
    const auto proj2 = factor.empty()
        ? proj_type {proj}
        : proj_type {[&](auto index) { return proj(index) * factor[idx]; }};

    for (size_t count = 0; auto&& sindex : active_sceneries) {
      for (auto&& tindex : active_stages) {
        auto&& stiter = hstb.find({sindex, tindex});
        const auto has_stindex =
            stiter != hstb.end() && !stiter->second.empty();

        for (auto&& bindex : active_stage_blocks[tindex]) {
          if (has_stindex) {
            values[idx] = proj2(stiter->second.at(bindex));
            valid[idx] = true;
            ++count;

            need_values = true;
          }
          need_valids |= count != ++idx;
        }
      }
    }

    return std::make_pair(need_values ? values : std::vector<double> {},
                          need_valids ? valid : std::vector<bool> {});
  }

  template<typename Projection, typename Span = std::span<double>>
  constexpr auto flat(const STIndexHolder& hst,
                      Projection proj,
                      const Span& factor = {}) const
  {
    const auto size = active_scenery_count() * active_stage_count();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    using proj_type = std::function<double(size_t)>;
    const auto proj2 = factor.empty()
        ? proj_type {proj}
        : proj_type {[&](auto index) { return proj(index) * factor[idx]; }};

    for (size_t count = 0; auto&& sindex : active_sceneries) {
      for (auto&& tindex : active_stages) {
        auto&& stiter = hst.find({sindex, tindex});
        const auto has_stindex = stiter != hst.end();

        if (has_stindex) {
          values[idx] = proj2(stiter->second);
          valid[idx] = true;
          ++count;

          need_values = true;
        }
        need_valids |= count != ++idx;
      }
    }

    return std::make_pair(need_values ? values : std::vector<double> {},
                          need_valids ? valid : std::vector<bool> {});
  }

  template<typename Projection = std::identity,
           typename Span = std::span<double>>
  constexpr auto flat(const TIndexHolder& ht,
                      Projection proj = {},
                      const Span& factor = {}) const
  {
    const auto size = active_stage_count();
    std::vector<double> values(size);
    std::vector<bool> valid(size, false);

    bool need_values = false;
    bool need_valids = false;

    size_t idx = 0;
    using proj_type = std::function<double(size_t)>;
    const auto proj2 = factor.empty()
        ? proj_type {proj}
        : proj_type {[&](auto index) { return proj(index) * factor[idx]; }};

    for (size_t count = 0; auto&& tindex : active_stages) {
      auto&& titer = ht.find(tindex);
      const auto has_tindex = titer != ht.end();

      if (has_tindex) {
        values[idx] = proj2(titer->second);
        valid[idx] = true;
        ++count;

        need_values = true;
      }
      need_valids |= count != ++idx;
    }

    return std::make_pair(need_values ? values : std::vector<double> {},
                          need_valids ? valid : std::vector<bool> {});
  }

  template<typename Max>
  constexpr auto block_max_at(const BlockIndex block_index,
                              const Max& lmax,
                              const double capacity_max = CoinDblMax) const
  {
    const auto lmax_at =
        lmax.at(stage_index(), block_index).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return lmax_block;
  }

  template<typename Min, typename Max>
  constexpr auto block_maxmin_at(const BlockIndex block_index,
                                 const Max& lmax,
                                 const Min& lmin,
                                 const double capacity_max,
                                 const double capacity_min = 0.0) const
      -> std::pair<double, double>
  {
    const auto lmin_at =
        lmin.at(stage_index(), block_index).value_or(capacity_min);
    const auto lmin_block = std::max(capacity_min, lmin_at);

    const auto lmax_at =
        lmax.at(stage_index(), block_index).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return {lmax_block, lmin_block};
  }

  template<typename Min, typename Max>
  constexpr auto stage_maxmin_at(const Min& lmax,
                                 const Max& lmin,
                                 const double capacity_max,
                                 const double capacity_min = 0.0) const
      -> std::pair<double, double>
  {
    const auto lmin_at = lmin.at(stage_index()).value_or(capacity_min);
    const auto lmin_block = std::max(capacity_min, lmin_at);

    const auto lmax_at = lmax.at(stage_index()).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return {lmax_block, lmin_block};
  }

  //
  // set&get the variable data
  //
  constexpr void set_scenery(const SceneryIndex sindex,
                             const SceneryLP& scenery)
  {
    m_scenery_index_ = sindex;
    m_scenery_uid_ = scenery.uid();
    m_scenery_probability_factor_ = scenery.probability_factor();
  }

  void set_stage(const StageIndex tindex, const StageLP& stage)
  {
    m_stage_index_ = tindex;
    m_stage_uid_ = stage.uid();
    m_stage_blocks_ = stage.blocks();
    m_stage_block_indexes_ = stage.indexes();
    m_stage_duration_ = stage.duration();
    m_stage_discount_factor_ = stage_discount_factors.at(tindex);
  }

  [[nodiscard]] constexpr auto&& single_bus_id() const
  {
    return m_single_bus_id_;
  }

  constexpr bool is_single_bus(const auto& id) const
  {
    if (m_single_bus_id_) {
      auto&& sid = m_single_bus_id_.value();
      return sid.index() == 0 ? std::get<0>(sid) == id.first
                              : std::get<1>(sid) == id.second;
    }
    return false;
  }

  [[nodiscard]] auto get_bus_index(const ObjectSingleId<BusLP>& id) const
      -> ElementIndex<BusLP>;
  [[nodiscard]] auto get_bus(const ObjectSingleId<BusLP>& id) const
      -> const BusLP&;

  template<typename Element>
  constexpr auto&& elements() const
  {
    return get_elements<Element>(*this);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto&& element(const Id<Element>& id) const
  {
    return get_element(*this, id);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto element_index(const Id<Element>& id) const
  {
    return get_element_index(*this, id);
  }

  template<typename Element>
  constexpr auto add_element(Element&& element)
  {
    return push_back(*this, std::forward<Element>(element));
  }

  [[nodiscard]] constexpr auto system() -> SystemLP& { return m_system_.get(); }
  [[nodiscard]] constexpr auto system() const -> const SystemLP&
  {
    return m_system_.get();
  }

  [[nodiscard]] constexpr auto get_scenery_size() const { return scenery_size; }
  [[nodiscard]] constexpr auto get_stage_size() const { return stage_size; }
  [[nodiscard]] constexpr auto get_block_size() const { return block_size; }

  [[nodiscard]] auto sceneries() const -> const std::vector<SceneryLP>&;
  [[nodiscard]] auto stages() const -> const std::vector<StageLP>&;
  [[nodiscard]] auto blocks() const -> const std::vector<BlockLP>&;

  template<typename Map, typename BHolder>
  constexpr auto emplace_bholder(Map& map,
                                 BHolder&& holder,
                                 bool empty_insert = false) const
  {
    // if empty_insert, holders that are empty will be inserted. Otherwise,
    // there will be skipped.

    // holder.shrink_to_fit();

    using Key = typename Map::key_type;
    return (empty_insert || !holder.empty())
        ? map.emplace(Key {m_scenery_index_, m_stage_index_},
                      std::forward<BHolder>(holder))
        : std::make_pair(map.end(), true);
  }

  template<typename Map>
  constexpr auto emplace_value(Map& map, size_t value) const
  {
    using Key = typename Map::key_type;
    return map.emplace(Key {m_scenery_index_, m_stage_index_}, value);
  }

  template<typename Map>
  constexpr auto emplace_stage_value(Map&& map, size_t value) const
  {
    map.emplace(m_stage_index_, value);
    return std::forward(map);
  }

private:
  std::reference_wrapper<SystemLP> m_system_;
  std::vector<SceneryIndex> active_sceneries;
  std::vector<StageIndex> active_stages;
  std::vector<BlockIndex> active_blocks;
  std::vector<std::vector<BlockIndex>> active_stage_blocks;

  size_t scenery_size;
  size_t stage_size;
  size_t block_size;

  std::vector<double> stage_discount_factors;

  /// variable members
  mutable std::optional<ObjectSingleId<BusLP>> m_single_bus_id_ {};
  SceneryIndex m_scenery_index_;
  SceneryUid m_scenery_uid_;
  double m_scenery_probability_factor_ {1};

  StageUid m_stage_uid_;
  StageIndex m_stage_index_;
  double m_stage_duration_ {0};
  double m_stage_discount_factor_ {1};
  StageLP::BlockSpan m_stage_blocks_ {};
  StageLP::BlockIndexSpan m_stage_block_indexes_ {};
};

}  // namespace gtopt
