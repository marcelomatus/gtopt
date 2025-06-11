/**
 * @file      input_traits.hpp
 * @brief     Input data access traits
 * @date      Mon Mar 24 01:48:02 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines traits for input data access
 */

#pragma once

#include <gtopt/mvector_traits.hpp>
#include <gtopt/overload.hpp>  // Add Overload include
#include <gtopt/uid_traits.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/**
 * @brief Input data access traits
 *
 * Inherits from UidTraits and adds input-specific functionality
 */
struct InputTraits : UidTraits
{
  template<typename... Uid>
  using array_vector_uid_idx_v =
      std::variant<arrow_array_uid_idx_t<Uid...>, vector_uid_idx_t<Uid...>>;
  template<typename SystemContextType = class SystemContext>
  static auto read_table(const SystemContextType& sc,
                         std::string_view cname,
                         std::string_view fname) -> ArrowTable;

  template<typename Type, typename RType = Type, typename FSched, typename UidIdx, typename AccessOper, typename... Uid>
  static constexpr auto access_sched(const FSched& sched,
                                    [[maybe_unused]] const UidIdx& uid_idx,
                                    [[maybe_unused]] AccessOper access_oper,
                                    [[maybe_unused]] Uid... uid) -> RType
  {
    using value_type = Type;
    using vector_uid_idx_t = vector_uid_idx_t<Uid...>;
    using idx_key_t = typename UidToVectorIdx<Uid...>::IndexKey;
    using vector_traits = mvector_traits<value_type, idx_key_t>;
    using vector_type = typename vector_traits::vector_type;

    auto visitor = Overload {
      [](const value_type& val) -> RType {
        return RType{val};
      },
      [&](const vector_type& vec) -> RType {
        const auto& v_uid_idx = std::get<vector_uid_idx_t>(uid_idx);
        idx_key_t idx_key = v_uid_idx->at(std::make_tuple(uid...));
        return RType{vector_traits::at_value(vec, idx_key)};
      },
      [&](const auto& arr_idx) -> RType {
        using a_uid_idx_type = arrow_array_uid_idx_t<Uid...>;
        const auto& [array, a_uid_idx] = std::get<a_uid_idx_type>(uid_idx);
        if (!array || !a_uid_idx) {
          throw std::runtime_error("Invalid arrow array or index");
        }
        using array_value = typename arrow::CTypeTraits<Type>::ArrayType;
        return RType{
          access_oper(std::static_pointer_cast<array_value>(array->chunk(0)),
                     a_uid_idx,
                     std::make_tuple(uid...))
        };
      }
    };

    return std::visit(visitor, sched);
  }

  template<typename Type, typename FSched, typename UidIdx, typename... Uid>
  constexpr static auto at_sched(const FSched& sched,
                                const UidIdx& uid_idx,
                                Uid... uid) -> Type
  {
    return access_sched<Type>(sched, uid_idx, 
      []<typename Arr, typename Idx>(const Arr& values, const Idx& uid_idx, const auto& key) -> Type {
        return values->Value(uid_idx->at(key));
      }, 
      uid...);
  }

  template<typename Type, typename FSched, typename UidIdx, typename... Uid>
  constexpr static auto optval_sched(const FSched& sched,
                                    const UidIdx& uid_idx,
                                    Uid... uid) -> std::optional<Type>
  {
    return access_sched<Type, std::optional<Type>>(sched, uid_idx,
      []<typename Arr, typename Idx>(const Arr& values, const Idx& uid_idx, const auto& key) -> std::optional<Type> {
        return get_optvalue(*uid_idx, key)
          .transform([&values](auto idx) { return values->Value(idx); });
      },
      uid...);
  }
};

}  // namespace gtopt
