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

#include <format>
#include <stdexcept>
#include <utility>

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/input_traits.hpp>
#include <gtopt/overload.hpp>
#include <gtopt/scenery.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/system_context.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

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
        const auto str = std::format(
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
            std::format("can't find element {} in table {}", fname, name);
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

template<typename Map, typename FieldSched, typename... Index>
constexpr static auto make_array_index(const SystemContext& sc,
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

class SystemContext;

class InputContext : public InputTraits
{
public:
  template<typename SystemContextType = class SystemContext>
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
  std::reference_wrapper<SystemContext> sc;

  mutable std::tuple<array_table_map_t<SceneryIndex, StageIndex, BlockIndex>,
                     array_table_map_t<SceneryIndex, StageIndex>,
                     array_table_map_t<StageIndex, BlockIndex>,
                     array_table_map_t<StageIndex>>
      m_array_table_maps_;
};

}  // namespace gtopt
