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

template<typename Type, typename Map, typename FieldSched, typename... Uid>
struct ArrayIndexTraits : InputTraits
{
private:
  constexpr static auto get_arrow_index(const auto& system_context,
                                       std::string_view cname,
                                       std::string_view fname,
                                       const Id& id,
                                       auto& array_map,
                                       auto& table_map)
  {
    const auto& [uid, name] = id;
    using array_map_key = typename std::decay_t<decltype(array_map)>::key_type;
    using table_map_key = typename std::decay_t<decltype(table_map)>::key_type;

    // Try to find existing array first (cache hit)
    if (auto aiter = array_map.find(array_map_key{cname, fname, uid}); 
        aiter != array_map.end()) {
      return aiter->second;
    }

    // Cache miss - need to load table and create index
    auto [table, index_idx] = [&]() -> std::pair<ArrowTable, ArrowUidIdx<Uid...>> {
      auto table_key = table_map_key{cname, fname};
      
      if (auto titer = table_map.find(table_key); titer != table_map.end()) {
        return titer->second;
      }

      // Load table and create index if not found
      auto table = read_arrow_table(system_context, cname, fname);
      auto index = UidToArrowIdx<Uid...>::make_arrow_uids_idx(table);
      
      if (!table || !index) [[unlikely]] {
        const auto msg = std::format("Can't create table/index for {}:{}", fname, name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      if (!table_map.emplace(table_key, std::pair{table, index}).second) [[unlikely]] {
        throw std::runtime_error("Can't insert non-unique table key");
      }

      return {table, index};
    }();

    // Try multiple column name patterns
    auto array = [&]() -> ArrowChunkedArray {
      if (auto col = table->GetColumnByName(std::string{name})) {
        return col;
      }
      if (auto col = table->GetColumnByName(as_label<':'>(name, uid))) {
        return col;
      }
      return table->GetColumnByName(as_label<':'>("uid", uid));
    }();

    if (!array) [[unlikely]] {
      const auto msg = std::format("Can't find column {} in table {}", name, fname);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    // Cache the array
    if (!array_map.emplace(array_map_key{cname, fname, uid}, 
                          std::pair{array, index_idx}).second) [[unlikely]] {
      throw std::runtime_error("Can't insert non-unique array key");
    }

    return {array, index_idx};
  }

public:
  using value_type = Type;
  using uid_tuple = std::tuple<Uid...>;
  using vector_traits = mvector_traits<value_type, uid_tuple>;
  using vector_type = typename vector_traits::vector_type;
  using array_vector_uid_idx_v = InputTraits::array_vector_uid_idx_v<Uid...>;

  static auto make_array_index(const auto& system_context,
                               const std::string_view class_name,
                               Map& array_table_map,
                               const FieldSched& sched,
                               const Id& id) -> array_vector_uid_idx_v
  {
    using map_type = array_table_vector_uid_idx_t<Uid...>;

    auto&& [array_map, table_map, vector_idx] =
        std::get<map_type>(array_table_map);

    return std::visit(
        Overload {
            [&](const value_type&) -> array_vector_uid_idx_v { return {}; },
            [&](const vector_type&) -> array_vector_uid_idx_v
            {
              if (!vector_idx) {
                vector_idx = UidToVectorIdx<Uid...>::make_vector_uids_idx(
                    system_context.simulation());
              }
              return {vector_idx};
            },
            [&](const FileSched& fsched) -> array_vector_uid_idx_v
            {
              return get_arrow_index(
                  system_context, class_name, fsched, id, array_map, table_map);
            }},
        sched);
  }
};

template<typename Type, typename Map, typename FieldSched, typename... Uid>
constexpr auto make_array_index(const auto& system_context,
                                const std::string_view ClassName,
                                Map& array_table_map,
                                const FieldSched& sched,
                                const Id& id)
{
  return ArrayIndexTraits<Type, Map, FieldSched, Uid...>::make_array_index(
      system_context, ClassName, array_table_map, sched, id);
}

}  // namespace gtopt
