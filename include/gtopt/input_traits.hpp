/**
 * @file      uinput_traits.hpp
 * @brief     Header of
 * @date      Mon Mar 24 01:48:02 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

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
#include <gtopt/uididx_traits.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

struct UidTraits
{
  template<typename Key, typename Value>
  using base_map_t = gtopt::flat_map<Key, Value>;

  using ClassNameType = std::string_view;
  using FieldNameType = std::string_view;
  using CFNameUid = std::tuple<ClassNameType, FieldNameType, Uid>;
  using CFName = std::tuple<ClassNameType, FieldNameType>;

  using ArrowChunkedArray = gtopt::ArrowChunkedArray;
  using ArrowTable = gtopt::ArrowTable;

  template<typename... Uid>
  using ArrowUidIdx = typename UidToArrowIdx<Uid...>::UidIdx;

  template<typename... Uid>
  using arrow_array_uid_idx_t =
      std::tuple<ArrowChunkedArray, ArrowUidIdx<Uid...>>;

  template<typename... Uid>
  using array_uid_idx_map_t =
      base_map_t<CFNameUid, arrow_array_uid_idx_t<Uid...>>;

  template<typename... Uid>
  using arrow_table_uid_idx_t = std::tuple<ArrowTable, ArrowUidIdx<Uid...>>;

  template<typename... Uid>
  using arrow_table_map_t = base_map_t<CFName, arrow_table_uid_idx_t<Uid...>>;

  template<typename... Uid>
  using table_uid_idx_map_t = base_map_t<CFName, arrow_table_uid_idx_t<Uid...>>;

  template<typename... Uid>
  using VectorUidIdx = typename UidToVectorIdx<Uid...>::UidIdx;

  template<typename... Uid>
  using vector_uid_idx_t = VectorUidIdx<Uid...>;

  template<typename... Uid>
  using idx_key_t = typename UidToVectorIdx<Uid...>::IndexKey;

  template<typename value_type, typename... Uid>
  using idx_vector_t =
      typename mvector_traits<value_type, idx_key_t<Uid...>>::vector_type;

  template<typename... Uid>
  using array_table_vector_uid_idx_t = std::tuple<array_uid_idx_map_t<Uid...>,
                                                  table_uid_idx_map_t<Uid...>,
                                                  vector_uid_idx_t<Uid...>>;
  template<typename... Uid>
  using array_vector_uid_idx_v =
      std::variant<arrow_array_uid_idx_t<Uid...>, vector_uid_idx_t<Uid...>>;
};

struct InputTraits : UidTraits
{
  template<typename SystemContextType = class SystemContext>
  static auto read_table(const SystemContextType& sc,
                         std::string_view cname,
                         std::string_view fname) -> ArrowTable;

  template<typename Type,
           typename RType = Type,
           typename FSched,
           typename UidIdx,
           typename AccessOper,
           typename... Uid>
  static constexpr auto access_sched(const FSched& sched,
                                     const UidIdx& uid_idx,
                                     AccessOper access_oper,
                                     Uid... uid)
  {
    using value_type = Type;
    using vector_uid_idx_t = vector_uid_idx_t<Uid...>;
    using idx_key_t = typename UidToVectorIdx<Uid...>::IndexKey;
    using vector_traits = mvector_traits<value_type, idx_key_t>;
    using vector_type = typename vector_traits::vector_type;

    if (std::holds_alternative<value_type>(sched)) [[likely]] {
      return RType {std::get<value_type>(sched)};
    } else if (std::holds_alternative<vector_type>(sched)) [[unlikely]] {
      const auto& v_uid_idx = std::get<vector_uid_idx_t>(uid_idx);
      idx_key_t idx_key = v_uid_idx->at(std::make_tuple(uid...));
      return RType {
          vector_traits::at_value(std::get<vector_type>(sched), idx_key)};
    } else {
      using a_uid_idx_type = arrow_array_uid_idx_t<Uid...>;
      const auto& [array, a_uid_idx] = std::get<a_uid_idx_type>(uid_idx);
      if (array && a_uid_idx) {
        using array_value = typename arrow::CTypeTraits<Type>::ArrayType;
        return access_oper(
            std::static_pointer_cast<array_value>(array->chunk(0)),
            a_uid_idx,
            std::make_tuple(uid...));
      }
    }

    throw std::runtime_error("bad created or form schedule");
  }

  template<typename Type, typename FSched, typename UidIdx, typename... Uid>
  constexpr static auto at_sched(const FSched& sched,
                                 const UidIdx& uid_idx,
                                 Uid... uid) -> Type
  {
    constexpr auto access_oper = [](const auto& values,
                                    const auto& uid_idx,
                                    const auto& key) constexpr -> Type
    { return values->Value(uid_idx->at(key)); };

    return access_sched<Type>(sched, uid_idx, access_oper, uid...);
  }

  template<typename Type, typename FSched, typename UidIdx, typename... Uid>
  constexpr static auto optval_sched(const FSched& sched,
                                     const UidIdx& uid_idx,
                                     Uid... uid) -> std::optional<Type>
  {
    constexpr auto access_oper =
        [](const auto& values,
           const auto& uid_idx,
           const auto& key) constexpr -> std::optional<Type>
    {
      const auto oidx = get_optvalue(*uid_idx, key);
      if (oidx) {
        return values->Value(*oidx);
      }
      return {};
    };

    return access_sched<Type, std::optional<Type>>(
        sched, uid_idx, access_oper, uid...);
  }
};

}  // namespace gtopt
