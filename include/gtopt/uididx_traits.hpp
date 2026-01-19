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

#include <expected>
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
struct UidColumn
{
  [[nodiscard]]
  static constexpr auto make_uid_column(const ArrowTable& table,
                                        const std::string& name) noexcept
      -> std::expected<std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType>,
                       std::string>
  {
    if (!table) {
      auto msg = fmt::format("Null table, no column for name '{}'", name);
      SPDLOG_ERROR(msg);
      return std::unexpected(std::move(msg));
    }

    const auto column = table->GetColumnByName(name);
    if (!column) {
      auto msg = fmt::format("Column '{}' not found in table", name);
      SPDLOG_ERROR(msg);
      return std::unexpected(std::move(msg));
    }

    try {
      if (column->num_chunks() == 0) {
        auto msg = fmt::format("Column '{}' has no chunks", name);
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }

      const auto chunk = column->chunk(0);
      if (!chunk) {
        auto msg = fmt::format("Null chunk in column '{}'", name);
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }

      if (chunk->type_id() != ArrowTraits<Uid>::Type::type_id) {
        auto msg =
            fmt::format("Type mismatch in column '{}': expected {} got {}",
                        name,
                        ArrowTraits<Uid>::Type::type_name(),
                        chunk->type()->ToString());
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }

      return std::static_pointer_cast<arrow::CTypeTraits<Uid>::ArrayType>(
          chunk);
    } catch (const std::exception& e) {
      auto msg = fmt::format("Column cast failed: {}", e.what());
      SPDLOG_ERROR(msg);
      return std::unexpected(std::move(msg));
    }
  }
};

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
    , UidColumn
{
  using BaseMapTraits = UidMapTraits<ArrowIndex, Uids...>;

  using typename BaseMapTraits::key_type;
  using uid_arrow_idx_map_t = BaseMapTraits::uid_map_t;
  using uid_arrow_idx_map_ptr = BaseMapTraits::uid_map_ptr;
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
  using UidIdx = uid_arrow_idx_map_ptr;

  static constexpr auto make_arrow_uids_idx(const ArrowTable& table)
  {
    const auto scenarios = make_uid_column(table, Scenario::class_name);
    if (!scenarios) {
      SPDLOG_ERROR(
          fmt::format("Failed to get scenarios column {}", scenarios.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }
    const auto stages = make_uid_column(table, Stage::class_name);
    if (!stages) {
      SPDLOG_ERROR(
          fmt::format("Failed to get stages column {}", stages.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }
    const auto blocks = make_uid_column(table, Block::class_name);
    if (!blocks) {
      SPDLOG_ERROR(
          fmt::format("Failed to get blocks column {}", blocks.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto key = key_type {ScenarioUid {(*scenarios)->Value(i)},
                                 StageUid {(*stages)->Value(i)},
                                 BlockUid {(*blocks)->Value(i)}};
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_WARN(fmt::format("using duplicate uid values at element {}",
                                as_string(key)));
      }
    }

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<>
struct UidToArrowIdx<StageUid, BlockUid> : ArrowUidTraits<StageUid, BlockUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  static constexpr auto make_arrow_uids_idx(const ArrowTable& table)
  {
    const auto stages = make_uid_column(table, Stage::class_name);
    if (!stages) {
      SPDLOG_ERROR(
          fmt::format("Failed to get stages column: {}", stages.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }
    const auto blocks = make_uid_column(table, Block::class_name);
    if (!blocks) {
      SPDLOG_ERROR(
          fmt::format("Failed to get blocks column {}", blocks.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto key = key_type {StageUid {(*stages)->Value(i)},
                                 BlockUid {(*blocks)->Value(i)}};
      SPDLOG_DEBUG(
          fmt::format("uididx Processing key: {} and {}", as_string(key), i));
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_WARN(fmt::format("using duplicated id values at element {}",
                                as_string(key)));
      }
    }

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<>
struct UidToArrowIdx<ScenarioUid, StageUid>
    : ArrowUidTraits<ScenarioUid, StageUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  static constexpr auto make_arrow_uids_idx(const ArrowTable& table)
  {
    const auto scenarios = make_uid_column(table, Scenario::class_name);
    if (!scenarios) {
      SPDLOG_ERROR(
          fmt::format("Failed to get scenarios column {}", scenarios.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }
    const auto stages = make_uid_column(table, Stage::class_name);
    if (!stages) {
      SPDLOG_ERROR(
          fmt::format("Failed to get stages column {}", stages.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto key = key_type {ScenarioUid {(*scenarios)->Value(i)},
                                 StageUid {(*stages)->Value(i)}};
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_WARN(fmt::format("using duplicate uid values at element {}",
                                as_string(key)));
      }
    }

    return std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx));
  }
};

template<>
struct UidToArrowIdx<StageUid> : ArrowUidTraits<StageUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  static constexpr auto make_arrow_uids_idx(const ArrowTable& table)
  {
    const auto stages = make_uid_column(table, Stage::class_name);
    if (!stages) {
      SPDLOG_ERROR(
          fmt::format("Failed to get stages column {}", stages.error()));
      return std::shared_ptr<uid_arrow_idx_map_t>();
    }

    uid_arrow_idx_map_t uid_idx;
    uid_idx.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto key = key_type {StageUid {(*stages)->Value(i)}};
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_WARN(fmt::format("using duplicate uid values at element {}",
                                as_string(key)));
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
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

  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  [[nodiscard]] static constexpr auto make_vector_uids_idx(
      const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    index_uids.reserve(sim.scenarios().size() * sim.blocks().size());

    for (const auto& [si, scenario] : enumerate<Index>(sim.scenarios())) {
      for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
        for (const auto& [bi, block] : enumerate<Index>(stage.blocks())) {
          const auto res = index_uids.emplace(
              UidKey {scenario.uid(), stage.uid(), block.uid()},
              IndexKey {si, ti, bi});
          if (!res.second) {
            SPDLOG_WARN("using duplicate uid values");
          }
        }
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<StageUid, BlockUid>
{
  using IndexKey = std::tuple<Index, Index>;
  using UidKey = std::tuple<StageUid, BlockUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  static constexpr auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    index_uids.reserve(sim.blocks().size());

    for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
      for (const auto& [bi, block] : enumerate<Index>(stage.blocks())) {
        const auto res = index_uids.emplace(UidKey {stage.uid(), block.uid()},
                                            IndexKey {ti, bi});
        if (!res.second) {
          SPDLOG_WARN("using duplicate uid values");
        }
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<ScenarioUid, StageUid>
{
  using IndexKey = std::tuple<Index, Index>;
  using UidKey = std::tuple<ScenarioUid, StageUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  static constexpr auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    index_uids.reserve(sim.scenarios().size() * sim.stages().size());

    for (const auto& [si, scenario] : enumerate<Index>(sim.scenarios())) {
      for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
        const auto res = index_uids.emplace(
            UidKey {scenario.uid(), stage.uid()}, IndexKey {si, ti});
        if (!res.second) {
          SPDLOG_WARN("Duplicate uid values");
        }
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<StageUid>
{
  using IndexKey = std::tuple<Index>;
  using UidKey = std::tuple<StageUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;
  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  template<typename SystemContextType = class SystemContext>
  static auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    index_uids.reserve(sim.stages().size());

    for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
      const auto res = index_uids.emplace(UidKey {stage.uid()}, IndexKey {ti});
      if (!res.second) {
        SPDLOG_WARN("Duplicate uid values");
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

}  // namespace gtopt
