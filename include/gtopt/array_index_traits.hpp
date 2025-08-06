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

struct ArrayIndexBase
{
  [[nodiscard]] static auto read_arrow_table(const SystemContext& sc,
                                             std::string_view cname,
                                             std::string_view fname)
      -> ArrowTable;

  template<typename... Uid>
  [[nodiscard]] static constexpr auto get_arrow_index(
      const SystemContext& system_context,
      const std::string_view cname,
      const std::string_view fname,
      const Id& id,
      auto& array_map,
      auto& table_map)
  {
    using array_map_key = typename std::decay_t<decltype(array_map)>::key_type;
    using table_map_key = typename std::decay_t<decltype(table_map)>::key_type;

    const auto& [uid, name] = id;

    const auto array_key = array_map_key {cname, fname, uid};

    // Try to find existing array first (cache hit)
    if (auto aiter = array_map.find(array_key); aiter != array_map.end()) {
      return aiter->second;
    }

    // Cache miss - need to load table and create index
    auto [table, index_idx] = [&]()
    {
      const auto table_key = table_map_key {cname, fname};

      if (auto titer = table_map.find(table_key); titer != table_map.end()) {
        return titer->second;
      }

      // Load table and create index if not found
      auto table = read_arrow_table(system_context, cname, fname);
      auto index = UidToArrowIdx<Uid...>::make_arrow_uids_idx(table);

      if (!table || !index) [[unlikely]] {
        const auto msg =
            fmt::format("Can't create table/index for '{}:{}'", fname, name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      auto [res, ok] = table_map.emplace(table_key, std::pair {table, index});
      if (!ok) [[unlikely]] {
        const auto msg = fmt::format(
            "Can't insert non-unique table key '{}' '{}'", cname, fname);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      return res->second;
    }();

    // Try multiple column name patterns
    auto array = [&]()
    {
      if (auto col = table->GetColumnByName(std::string {name})) {
        return col;
      }
      if (auto col = table->GetColumnByName(as_label<':'>(name, uid))) {
        return col;
      }
      return table->GetColumnByName(as_label<':'>("uid", uid));
    }();

    if (!array) [[unlikely]] {
      const auto msg = fmt::format(
          "Can't find element '{}:{}' in table '{}'", name, uid, fname);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    // Cache the array
    auto [res, ok] = array_map.emplace(array_key, std::pair {array, index_idx});
    if (!ok) [[unlikely]] {
      const auto msg = fmt::format(
          "Can't insert non-unique arrow key '{} {} {}'", cname, fname, uid);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    return res->second;
  }
};

template<typename Type, typename Map, typename FSched, typename... Uid>
struct ArrayIndexTraits
    : InputTraits
    , ArrayIndexBase
{
private:
public:
  using uid_tuple = std::tuple<Uid...>;

  using value_type = Type;
  using vector_type = typename InputTraits::idx_vector_t<value_type, Uid...>;
  using file_sched = FileSched;

  using array_vector_uid_idx_v = InputTraits::array_vector_uid_idx_v<Uid...>;

  static constexpr auto make_array_index(const auto& system_context,
                                         const std::string_view class_name,
                                         Map& array_table_map,
                                         const FSched& sched,
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
            [&](const file_sched& fsched) -> array_vector_uid_idx_v
            {
              return get_arrow_index<Uid...>(
                  system_context, class_name, fsched, id, array_map, table_map);
            }},
        sched);
  }
};

template<typename Type, typename Map, typename FieldSched, typename... Uid>
constexpr auto make_array_index(const SystemContext& system_context,
                                const std::string_view class_name,
                                Map& array_table_map,
                                const FieldSched& sched,
                                const Id& id)
{
  return ArrayIndexTraits<Type, Map, FieldSched, Uid...>::make_array_index(
      system_context, class_name, array_table_map, sched, id);
}

}  // namespace gtopt
