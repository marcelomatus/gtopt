/**
 * @file      input_traits.hpp
 * @brief     Header of
 * @date      Mon Mar 24 01:48:02 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <tuple>

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/scenery.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

template<typename Value, typename... Index>
struct MapTraits
{
  using value_type = Value;
  using key_type = std::tuple<Index...>;

  using index_idx_t = gtopt::flat_map<key_type, value_type>;
  using IndexIdx = std::shared_ptr<index_idx_t>;
};

template<typename... Index>
struct IndexTraits
    : ArrowTraits<Uid>
    , MapTraits<ArrowIndex, Index...>
{
  template<typename Key, typename Value>
  using base_map_t = gtopt::flat_map<Key, Value>;

  static auto make_uid_column(const ArrowTable& table,
                              const std::string_view& name)
  {
    auto&& column = table->GetColumnByName(std::string {name});
    return std::static_pointer_cast<arrow::CTypeTraits<Uid>::ArrayType>(
        column ? column->chunk(0) : decltype(column->chunk(0)) {});
  }
};

template<typename... Index>
struct IndexToIdx : IndexTraits<Index...>
{
  using BaseIndexTraits = IndexTraits<Index...>;
  using typename BaseIndexTraits::index_idx_t;
  using typename BaseIndexTraits::IndexIdx;
};

class SystemContext;

template<>
struct IndexToIdx<SceneryIndex, StageIndex, BlockIndex>
    : IndexTraits<SceneryIndex, StageIndex, BlockIndex>
{
  template<typename SystemContextType = class SystemContext>
  static auto make_index_idx(const SystemContextType& sc,
                             const ArrowTable& table)
  {
    const auto sceneries = make_uid_column(table, Scenery::column_name);
    const auto stages = make_uid_column(table, Stage::column_name);
    const auto blocks = make_uid_column(table, Block::column_name);

    base_map_t<std::tuple<Uid, Uid, Uid>, ArrowIndex> index_uids;
    index_uids.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      decltype(index_uids)::key_type key {
          sceneries->Value(i), stages->Value(i), blocks->Value(i)};

      if (!index_uids.emplace(key, i).second) {
        throw std::runtime_error("can't insert non-unique key");
      }
    }

    index_idx_t index_idx;
    index_idx.reserve(index_uids.size());

    for (auto&& [si, scenery] : enumerate<SceneryIndex>(sc.sceneries())) {
      for (auto&& [ti, stage] : enumerate<StageIndex>(sc.stages())) {
        for (auto&& [bi, block] : enumerate<BlockIndex>(stage.blocks())) {
          decltype(index_uids)::key_type uid_key {
              scenery.uid(), stage.uid(), block.uid()};
          decltype(index_idx)::key_type idx_key {si, ti, bi};

          if (!index_idx.emplace(idx_key, index_uids.at(uid_key)).second) {
            throw std::runtime_error("can't insert non-unique key");
          }
        }
      }
    }

    return std::make_shared<index_idx_t>(std::move(index_idx));
  }
};

template<>
struct IndexToIdx<StageIndex, BlockIndex> : IndexTraits<StageIndex, BlockIndex>
{
  template<typename SystemContextType = class SystemContext>
  static auto make_index_idx(const SystemContextType& sc,
                             const ArrowTable& table)
  {
    const auto stages = make_uid_column(table, Stage::column_name);
    const auto blocks = make_uid_column(table, Block::column_name);

    base_map_t<std::tuple<Uid, Uid>, ArrowIndex> index_uids;
    index_uids.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      decltype(index_uids)::key_type key {stages->Value(i), blocks->Value(i)};

      if (!index_uids.emplace(key, i).second) {
        throw std::runtime_error("can't insert non-unique key");
      }
    }

    index_idx_t index_idx;
    index_idx.reserve(index_uids.size());

    for (auto&& [ti, stage] : enumerate<StageIndex>(sc.stages())) {
      for (auto&& [bi, block] : enumerate<BlockIndex>(stage.blocks())) {
        decltype(index_uids)::key_type uid_key {stage.uid(), block.uid()};
        decltype(index_idx)::key_type idx_key {ti, bi};

        if (!index_idx.emplace(idx_key, index_uids.at(uid_key)).second) {
          throw std::runtime_error("can't insert non-unique key");
        }
      }
    }

    return std::make_shared<index_idx_t>(std::move(index_idx));
  }
};

template<>
struct IndexToIdx<SceneryIndex, StageIndex>
    : IndexTraits<SceneryIndex, StageIndex>
{
  template<typename SystemContextType>
  static auto make_index_idx(const SystemContextType& sc,
                             const ArrowTable& table)
  {
    const auto sceneries = make_uid_column(table, Scenery::column_name);
    const auto stages = make_uid_column(table, Stage::column_name);

    base_map_t<std::tuple<Uid, Uid>, ArrowIndex> index_uids;
    index_uids.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      decltype(index_uids)::key_type key {sceneries->Value(i),
                                          stages->Value(i)};

      if (!index_uids.emplace(key, i).second) {
        throw std::runtime_error("can't insert non-unique key");
      }
    }

    index_idx_t index_idx;
    index_idx.reserve(index_uids.size());

    for (auto&& [si, scenery] : enumerate<SceneryIndex>(sc.sceneries())) {
      for (auto&& [ti, stage] : enumerate<StageIndex>(sc.stages())) {
        decltype(index_uids)::key_type uid_key {scenery.uid(), stage.uid()};
        decltype(index_idx)::key_type idx_key {si, ti};

        if (!index_idx.emplace(idx_key, index_uids.at(uid_key)).second) {
          throw std::runtime_error("can't insert non-unique key");
        }
      }
    }

    return std::make_shared<index_idx_t>(std::move(index_idx));
  }
};

template<>
struct IndexToIdx<StageIndex> : IndexTraits<StageIndex>
{
  template<typename SystemContextType = class SystemContext>
  static auto make_index_idx(const SystemContextType& sc,
                             const ArrowTable& table)
  {
    const auto stages = make_uid_column(table, Stage::column_name);

    base_map_t<std::tuple<Uid>, ArrowIndex> index_uids;
    index_uids.reserve(static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      decltype(index_uids)::key_type key {stages->Value(i)};

      if (!index_uids.emplace(key, i).second) {
        throw std::runtime_error("can't insert non-unique key");
      }
      index_uids[{stages->Value(i)}] = i;
    }

    index_idx_t index_idx;
    index_idx.reserve(index_uids.size());

    for (auto&& [ti, stage] : enumerate<StageIndex>(sc.stages())) {
      decltype(index_uids)::key_type uid_key {stage.uid()};
      decltype(index_idx)::key_type idx_key {ti};

      if (!index_idx.emplace(idx_key, index_uids.at(uid_key)).second) {
        throw std::runtime_error("can't insert non-unique key");
      }
    }

    return std::make_shared<index_idx_t>(std::move(index_idx));
  }
};

template<typename Type, typename... Index>
struct mvector_traits
{
};

template<typename Type, typename Index>
struct mvector_traits<Type, Index>
{
  using value_type = Type;
  using vector_type = std::vector<Type>;

  template<typename Container>
  constexpr static auto at_value(const Container& vec, Index idx)
  {
    return vec.at(idx);
  }
};

template<typename Type, typename I1, typename... Index>
struct mvector_traits<Type, I1, Index...>
{
  using value_type = Type;
  using vector_type =
      std::vector<typename mvector_traits<Type, Index...>::vector_type>;

  template<typename Container>
  constexpr static auto at_value(const Container& vec, I1 idx1, Index... idx)
  {
    return mvector_traits<Type, Index...>::at_value(vec.at(idx1), idx...);
  }
};

struct InputTraits
{
  using ClassNameType = std::string_view;
  using FieldNameType = std::string_view;

  using CFName = std::tuple<ClassNameType, FieldNameType>;
  using CFNameUid = std::tuple<ClassNameType, FieldNameType, Uid>;

  using ArrowChunkedArray = gtopt::ArrowChunkedArray;
  template<typename... Index>
  using IndexIdx = typename IndexToIdx<Index...>::IndexIdx;

  template<typename Key, typename Value>
  using base_map_t = gtopt::flat_map<Key, Value>;

  template<typename... Index>
  using table_map_t =
      base_map_t<CFName, std::pair<ArrowTable, IndexIdx<Index...>>>;

  template<typename... Index>
  using array_map_t =
      base_map_t<CFNameUid, std::pair<ArrowChunkedArray, IndexIdx<Index...>>>;

  template<typename... Index>
  using array_table_map_t =
      std::tuple<array_map_t<Index...>, table_map_t<Index...>>;

  template<typename SystemContextType = class SystemContext>
  static auto read_table(const SystemContextType& sc,
                         const std::string_view& cname,
                         const std::string_view& fname) -> ArrowTable;

  template<typename... Index>
  using array_index_t = std::pair<ArrowChunkedArray, IndexIdx<Index...>>;

  template<typename Type,
           typename RType = Type,
           typename FSched,
           typename ArrayIndexIdx,
           typename AccessOper,
           typename... Index>
  constexpr static auto access_sched(const FSched& sched,
                                     const ArrayIndexIdx& array_index,
                                     AccessOper access_oper,
                                     Index... index)
  {
    using traits = mvector_traits<Type, Index...>;
    using value_type = Type;
    using vector_type = typename traits::vector_type;

    if (std::holds_alternative<value_type>(sched)) [[likely]] {
      return RType {std::get<value_type>(sched)};
    } else if (std::holds_alternative<vector_type>(sched)) {
      return RType {traits::at_value(std::get<vector_type>(sched), index...)};
    } else {
      const auto& [array, index_idx] = array_index;
      if (array && index_idx) {
        using array_value = typename arrow::CTypeTraits<Type>::ArrayType;
        return access_oper(
            std::static_pointer_cast<array_value>(array->chunk(0)),
            index_idx,
            std::make_tuple(index...));
      }
    }

    const std::string msg = "bad created or form schedule at";
    throw std::runtime_error(msg);
  }

  template<typename Type,
           typename FSched,
           typename ArrayIndexIdx,
           typename... Index>
  constexpr static auto at_sched(const FSched& sched,
                                 const ArrayIndexIdx& array_index,
                                 Index... index) -> Type
  {
    return access_sched<Type>(
        sched,
        array_index,
        [](const auto& values, const auto& index_idx, auto&& key) -> Type
        {
          return values->Value(index_idx->at(std::forward<decltype(key)>(key)));
        },
        index...);
  }

  template<typename Type,
           typename FSched,
           typename ArrayIndexIdx,
           typename... Index>
  constexpr static auto optval_sched(const FSched& sched,
                                     const ArrayIndexIdx& array_index,
                                     Index... index) -> std::optional<Type>
  {
    return access_sched<Type, std::optional<Type>>(
        sched,
        array_index,
        [](const auto& values,
           const auto& index_idx,
           auto&& key) -> std::optional<Type>
        {
          const auto oidx =
              get_optvalue(*index_idx, std::forward<decltype(key)>(key));
          if (oidx) {
            return values->Value(oidx.value());
          }
          return {};
        },
        index...);
  }
};

}  // namespace gtopt
