/**
 * @file      array_index_traits.hpp
 * @brief     Header of
 * @date      Thu Apr 24 22:05:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/as_label.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/input_traits.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

template<typename Map, typename FieldSched, typename... Index>
struct ArrayIndexTraits : InputTraits
{
  template<typename SystemContextType = class SystemContext>
  static auto make_array_index(const SystemContextType& system_context,
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
        table = read_table(system_context, cname, fname);
      }
      if (!index_idx) {
        index_idx = IndexToIdx<Index...>::make_index_idx(system_context, table);
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
struct ArrayIndexTraits<Map, std::optional<FieldSched>, Index...> : InputTraits
{
  template<typename SystemContextType = class SystemContext>
  constexpr static auto make_array_index(
      const SystemContextType& system_context,
      const std::string_view& ClassName,
      Map& map,
      const std::optional<FieldSched>& sched,
      const Id& id) -> std::pair<ArrowChunkedArray, IndexIdx<Index...>>
  {
    if (sched.has_value()) {
      return ArrayIndexTraits<Map, FieldSched, Index...>::make_array_index(
          system_context, ClassName, map, sched.value(), id);
    }

    return {};
  }
};

template<typename Map, typename FieldSched, typename... Index>
constexpr auto make_array_index(const SystemContext& system_context,
                                const std::string_view& ClassName,
                                Map& array_table_map,
                                const FieldSched& sched,
                                const Id& id)
    -> std::pair<InputTraits::ArrowChunkedArray,
                 InputTraits::IndexIdx<Index...>>
{
  return ArrayIndexTraits<Map, FieldSched, Index...>::make_array_index(
      system_context, ClassName, array_table_map, sched, id);
}

}  // namespace gtopt
