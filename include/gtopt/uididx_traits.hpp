/**
 * @file      uididx_traits.hpp
 * @brief     Header of
 * @date      Mon Jun  2 22:26:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <concepts>
#include <expected>
#include <ranges>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/mvector_traits.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{
template<typename Value, typename... Uids>
struct UidMapTraits
{
  using value_type = Value;
  using key_type = std::tuple<Uids...>;

  using uid_map_t = gtopt::flat_map<key_type, value_type>;
  using uid_map_ptr = std::shared_ptr<uid_map_t>;
};

template<typename... Uids>
struct ArrowUidTraits
    : ArrowTraits<Uid>
    , UidMapTraits<ArrowIndex, Uids...>
{
  using BaseMapTraits = UidMapTraits<ArrowIndex, Uids...>;

  using typename BaseMapTraits::key_type;
  using uid_arrow_idx_map_t = typename BaseMapTraits::uid_map_t;
  using uid_arrow_idx_map_ptr = typename BaseMapTraits::uid_map_ptr;

  [[nodiscard]] static constexpr auto make_uid_column(
      const ArrowTable& table, std::string_view name) noexcept
      -> std::expected<std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType>,
                       std::string>
  {
    if (!table) {
      return std::unexpected("Null table provided");
    }

    const auto& column = table->GetColumnByName(std::string {name});
    if (!column) {
      return std::unexpected(fmt::format("Column '{}' not found", name));
    }

    try {
      return std::static_pointer_cast<arrow::CTypeTraits<Uid>::ArrayType>(
          column->chunk(0));
    } catch (const std::exception& e) {
      return std::unexpected(fmt::format("Column cast failed: {}", e.what()));
    }
  }
};

template<typename... Uids>
struct UidToArrowIdx : ArrowUidTraits<Uids...>
{
  using BaseUidTraits = ArrowUidTraits<Uids...>;
  using typename BaseUidTraits::uid_idx_t;
  using typename BaseUidTraits::UidIdx;
};

template<>
struct UidToArrowIdx<ScenarioUid, StageUid, BlockUid>
    : ArrowUidTraits<ScenarioUid, StageUid, BlockUid>
{
  static auto make_uids_arrow_idx(const ArrowTable& table)
  {
    const auto scenarios = make_uid_column(table, Scenario::class_name);
    const auto stages = make_uid_column(table, Stage::class_name);
    const auto blocks = make_uid_column(table, Block::class_name);

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    std::ranges::for_each(
        std::views::iota(ArrowIndex {0}, table->num_rows()),
        [&](ArrowIndex i) constexpr noexcept
        {
          const auto res =
              uid_idx.emplace(key_type {ScenarioUid {(*scenarios)->Value(i)},
                                        StageUid {(*stages)->Value(i)},
                                        BlockUid {(*blocks)->Value(i)}},
                              i);
          if (!res.second) {
            SPDLOG_WARN(
                fmt::format("using duplicate uid values at element{}", i));
          }
        });

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<>
struct UidToArrowIdx<StageUid, BlockUid> : ArrowUidTraits<StageUid, BlockUid>
{
  static auto make_uids_idx(const ArrowTable& table)
  {
    const auto stages = make_uid_column(table, Stage::class_name);
    const auto blocks = make_uid_column(table, Block::class_name);

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    std::ranges::for_each(
        std::views::iota(ArrowIndex {0}, table->num_rows()),
        [&](ArrowIndex i) constexpr noexcept
        {
          const auto res =
              uid_idx.emplace(key_type {StageUid {(*stages)->Value(i)},
                                        BlockUid {(*blocks)->Value(i)}},
                              i);
          if (!res.second) {
            SPDLOG_WARN(
                fmt::format("using duplicate uid values at element{}", i));
          }
        });

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<>
struct UidToArrowIdx<ScenarioUid, StageUid>
    : ArrowUidTraits<ScenarioUid, StageUid>
{
  static auto make_uids_idx(const ArrowTable& table)
  {
    const auto scenarios = make_uid_column(table, Scenario::class_name);
    const auto stages = make_uid_column(table, Stage::class_name);

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    std::ranges::for_each(
        std::views::iota(ArrowIndex {0}, table->num_rows()),
        [&](ArrowIndex i) constexpr noexcept
        {
          const auto res =
              uid_idx.emplace(key_type {ScenarioUid {(*scenarios)->Value(i)},
                                        StageUid {(*stages)->Value(i)}},
                              i);
          if (!res.second) {
            SPDLOG_WARN(
                fmt::format("using duplicate uid values at element{}", i));
          }
        });

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<>
struct UidToArrowIdx<StageUid> : ArrowUidTraits<StageUid>
{
  static auto make_uids_idx(const ArrowTable& table)
  {
    const auto stages = make_uid_column(table, Stage::class_name);

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    std::ranges::for_each(
        std::views::iota(ArrowIndex {0}, table->num_rows()),
        [&](ArrowIndex i) constexpr noexcept
        {
          const auto res =
              uid_idx.emplace(key_type {StageUid {(*stages)->Value(i)}}, i);
          if (!res.second) {
            SPDLOG_WARN(
                fmt::format("using duplicate uid values at element{}", i));
          }
        });

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<typename... Uids>
struct UidToVectorIdx
{
};

template<>
struct UidToVectorIdx<ScenarioUid, StageUid, BlockUid>
{
  using IndexKey = std::tuple<Index, Index, Index>;
  using UidKey = std::tuple<ScenarioUid, StageUid, BlockUid>;

  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  template<typename SystemContextType = class SystemContext>
  constexpr static auto make_uids_vector_idx(const SimulationLP& sim) noexcept
  {
    uid_vector_idx_map_t index_uids;
    for (auto&& [si, scenario] : enumerate<Index>(sim.scenarios())) {
      for (auto&& [ti, stage] : enumerate<Index>(sim.stages())) {
        for (auto&& [bi, block] : enumerate<Index>(stage.blocks())) {
          const auto res = index_uids.emplace(
              UidKey {scenario.uid(), stage.uid(), block.uid()},
              IndexKey {si, ti, bi});
          if (!res.second) {
            SPDLOG_WARN(fmt::format("using duplicate uid values {}:{}:{}",
                                    scenario.uid(),
                                    stage.uid(),
                                    block.uid()));
          }
        }
      }
    }
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<ScenarioUid, StageUid>
{
  using IndexKey = std::tuple<Index, Index>;
  using UidKey = std::tuple<ScenarioUid, StageUid>;

  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  template<typename SystemContextType = class SystemContext>
  constexpr static auto make_uids_vector_idx(const SimulationLP& sim) noexcept
  {
    uid_vector_idx_map_t index_uids;

    for (auto&& [si, scenario] : enumerate<Index>(sim.scenarios())) {
      for (auto&& [ti, stage] : enumerate<Index>(sim.stages())) {
        const auto res = index_uids.emplace(
            UidKey {scenario.uid(), stage.uid()}, IndexKey {si, ti});
        if (!res.second) {
          SPDLOG_WARN(fmt::format("using duplicate uid values {}:{}:{}",
                                  scenario.uid(),
                                  stage.uid()));
        }
      }
    }
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<StageUid>
{
  using IndexKey = std::tuple<Index>;
  using UidKey = std::tuple<StageUid>;

  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  template<typename SystemContextType = class SystemContext>
  constexpr static auto make_uids_vector_idx(const SimulationLP& sim) noexcept
  {
    uid_vector_idx_map_t index_uids;
    for (auto&& [ti, stage] : enumerate<Index>(sim.stages())) {
      const auto res = index_uids.emplace(UidKey {stage.uid()}, IndexKey {ti});
      if (!res.second) {
        SPDLOG_WARN(
            fmt::format("using duplicate uid values {}:{}:{}", stage.uid()));
      }
    }
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

}  // namespace gtopt
