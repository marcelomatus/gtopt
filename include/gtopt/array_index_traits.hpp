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
  using value_type = Type;
  using uid_tuple = std::tuple<Uid...>;
  using vector_traits = mvector_traits<value_type, uid_tuple>;
  using vector_type = typename vector_traits::vector_type;

  using arrow_array_uid_idx_t = InputTraits::arrow_array_uid_idx_t<Uid...>;
  using vector_uid_idx_t = InputTraits::vector_uid_idx_t<Uid...>;
  using array_vector_uid_idx_v = InputTraits::array_vector_uid_idx_v<Uid...>;

  template<typename SystemContextType = class SystemContext>
  static auto make_array_index(const SystemContextType& system_context,
                               const std::string_view class_name,
                               Map& array_table_map,
                               const FieldSched& sched,
                               const Id& id) -> array_vector_uid_idx_v
  {
    if (std::holds_alternative<value_type>(sched)) [[likely]] {
      return {};
    }

    using map_type = array_table_vector_uid_idx_t<Uid...>;
    auto&& [array_map, table_map, vector_idx] =
        std::get<map_type>(array_table_map);

    if (std::holds_alternative<vector_type>(sched)) {
      if (!vector_idx) {
        vector_idx = UidToVectorIdx<Uid...>::make_vector_uids_idx(
            system_context.simulation());
      }
      return {vector_idx};
    }

    auto&& fsched = std::get<gtopt::FileSched>(sched);
    auto cname = class_name;
    auto&& fname = fsched;

    ArrowChunkedArray array;
    ArrowUidIdx<Uid...> index_idx;

    const auto& [uid, name] = id;
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
        index_idx = UidToArrowIdx<Uid...>::make_arrow_uids_idx(table);
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

    return std::make_tuple(array, index_idx);
  }
};

template<typename Type, typename Map, typename FieldSched, typename... Uid>
constexpr auto make_array_index(const SystemContext& system_context,
                                const std::string_view ClassName,
                                Map& array_table_map,
                                const FieldSched& sched,
                                const Id& id)
{
  return ArrayIndexTraits<Type, Map, FieldSched, Uid...>::make_array_index(
      system_context, ClassName, array_table_map, sched, id);
}

}  // namespace gtopt
