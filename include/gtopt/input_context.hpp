/**
 * @file      input_context.hpp
 * @brief     Header of
 * @date      Sat Mar 22 22:54:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <stdexcept>
#include <utility>

#include <fmt/core.h>
#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/overload.hpp>
#include <gtopt/scenery.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage.hpp>
#include <spdlog/spdlog.h>

// #include "system_context.hpp"

namespace gtopt
{

class SystemContext;

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
  template<typename SystemContextType = class SystemContext>
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

template<typename Map, typename FieldSched, typename... Index>
struct GetArrayIndex : InputTraits
{
  template<typename SystemContextType = class SystemContext>
  static auto make_array_index(const SystemContextType& sc,
                               const std::string_view& class_name,
                               Map& array_table_map,
                               const FieldSched& sched,
                               const Id& id)
      -> std::pair<ArrowChunkedArray, IndexIdx<Index...>>
  {
    ArrowChunkedArray array;
    IndexIdx<Index...> index_idx;

    if (!std::holds_alternative<gtopt::FileSched>(sched)) {
      return std::make_pair(array, index_idx);
    }

    auto&& fsched = std::get<gtopt::FileSched>(sched);
    auto cname = class_name;
    auto&& fname = fsched;

    using map_type = array_table_map_t<Index...>;

    auto&& [array_map, table_map] = std::get<map_type>(array_table_map);

    const auto [uid, name] = id;
    typename decltype(array_map)::key_type array_key {cname, fname, uid};

    const auto aiter = array_map.find(array_key);
    if (aiter != array_map.end()) {
      std::tie(array, index_idx) = aiter->second;
    }

    if (!array || !index_idx) {
      ArrowTable table {};

      typename decltype(table_map)::key_type table_key {cname, fname};

      const auto titer = table_map.find(table_key);
      if (titer != table_map.end()) {
        std::tie(table, index_idx) = titer->second;
      }

      if (!table) {
        table = read_table(sc, cname, fname);
      }
      if (!index_idx) {
        index_idx = IndexToIdx<Index...>::make_index_idx(sc, table);
      }

      if (table && index_idx) [[likely]] {
        if (titer == table_map.end()) {
          if (!table_map.emplace(table_key, std::make_pair(table, index_idx))
                   .second)
          {
            throw std::runtime_error("can't insert non-unique key");
          }
        }
      } else {
        const auto str = fmt::format(
            "can't create table or index for {} and {}", fname, name);

        SPDLOG_CRITICAL(str);
        throw std::runtime_error(str);
      }

      array = table->GetColumnByName(std::string {name});

      if (!array) {
        const auto col_name = as_label<':'>(name, uid);
        array = table->GetColumnByName(col_name);
      }

      if (!array) {
        const auto col_name = as_label<':'>("uid", uid);
        array = table->GetColumnByName(col_name);
      }

      if (array) [[likely]] {
        if (!array_map.emplace(array_key, std::make_pair(array, index_idx))
                 .second)
        {
          throw std::runtime_error("can't insert non-unique key");
        }
      } else {
        const auto str =
            fmt::format("can't find element {} in table {}", fname, name);
        SPDLOG_CRITICAL(str);
        throw std::runtime_error(str);
      }
    }

    return {array, index_idx};
  }
};

template<typename Map, typename FieldSched, typename... Index>
struct GetArrayIndex<Map, std::optional<FieldSched>, Index...> : InputTraits
{
  template<typename SystemContextType = class SystemContext>
  constexpr static auto make_array_index(const SystemContextType& sc,
                                         const std::string_view& ClassName,
                                         Map& map,
                                         const std::optional<FieldSched>& sched,
                                         const Id& id)
      -> std::pair<ArrowChunkedArray, IndexIdx<Index...>>
  {
    if (sched.has_value()) {
      return GetArrayIndex<Map, FieldSched, Index...>::make_array_index(
          sc, ClassName, map, sched.value(), id);
    }

    return {};
  }
};

template<typename SystemContextType,
         typename Map,
         typename FieldSched,
         typename... Index>
constexpr static auto make_array_index(const SystemContextType& sc,
                                       const std::string_view& ClassName,
                                       Map& array_table_map,
                                       const FieldSched& sched,
                                       const Id& id)
    -> std::pair<InputTraits::ArrowChunkedArray,
                 InputTraits::IndexIdx<Index...>>
{
  return GetArrayIndex<Map, FieldSched, Index...>::make_array_index(
      sc, ClassName, array_table_map, sched, id);
}

template<typename SystemContextType = class SystemContext>
class InputContext : public InputTraits
{
public:
  explicit InputContext(SystemContextType& psc)
      : sc(psc)
  {
  }

  template<typename FSched, typename... Index>
  auto get_array_index(const FSched& sched,
                       const std::string_view& cname,
                       const Id& id) const
      -> std::pair<ArrowChunkedArray, IndexIdx<Index...>>
  {
    return make_array_index<decltype(m_array_table_maps_), FSched, Index...>(
        sc.get(), cname, m_array_table_maps_, sched, id);
  }

  template<typename Element>
  auto&& elements() const
  {
    return sc.get().template elements<Element>();
  }

  template<typename Element, template<typename> class Id>
  auto element_index(const Id<Element>& id) const
  {
    return sc.get().element_index(id);
  }

  template<typename Element, template<typename> class Id>
  auto&& element(const Id<Element>& id) const
  {
    return sc.get().element(id);
  }

  template<typename Element>
  auto add_element(Element&& element)
  {
    return sc.get().add_element(std::forward<Element>(element));
  }

  template<typename Element, typename Object, typename Attrs>
  auto make_element_index(const Object& objori,
                          const std::variant<Uid, Name, Attrs>& element_var)
      -> ElementIndex<Element>
  {
    using ElementId = ObjectSingleId<Element>;
    using ObjElement = typename Element::object_type;
    return std::visit(
        Overload {[this](const Uid uid)
                  { return element_index(ElementId {uid}); },
                  [this](const Name& name)
                  { return element_index(ElementId {name}); },
                  [this, &objori](Attrs attrs)
                  {
                    ObjElement objele {.uid = objori.uid,
                                       .name = objori.name,
                                       .active = objori.active};
                    return add_element(Element {
                        *this, std::move(objele.set_attrs(std::move(attrs)))});
                  }},
        element_var);
  }

private:
  std::reference_wrapper<SystemContextType> sc;

  mutable std::tuple<array_table_map_t<SceneryIndex, StageIndex, BlockIndex>,
                     array_table_map_t<SceneryIndex, StageIndex>,
                     array_table_map_t<StageIndex, BlockIndex>,
                     array_table_map_t<StageIndex>>
      m_array_table_maps_;
};

}  // namespace gtopt
