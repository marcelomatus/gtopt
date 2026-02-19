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

#include <type_traits>

#include <gtopt/field_sched.hpp>
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
  template<typename Type,
           typename RType = Type,
           typename FSched,
           typename UidIdx,
           typename AccessOper,
           typename... Uid>
  static constexpr auto access_sched(const FSched& sched,
                                     const UidIdx& uid_idx,
                                     AccessOper access_oper,
                                     Uid... uid) -> RType
  {
    using value_type = Type;
    using vector_uid_idx_t = vector_uid_idx_t<Uid...>;
    using idx_key_t = UidToVectorIdx<Uid...>::IndexKey;
    using vector_traits = mvector_traits<value_type, idx_key_t>;
    using vector_type = vector_traits::vector_type;
    using file_sched = FileSched;

    auto visitor = Overload {
        [](const value_type& val) -> RType { return RType {val}; },
        [&](const vector_type& vec) -> RType
        {
          const auto& v_uid_idx = std::get<vector_uid_idx_t>(uid_idx);
          const idx_key_t idx_key = v_uid_idx->at(std::make_tuple(uid...));
          return RType {vector_traits::at_value(vec, idx_key)};
        },
        [&]([[maybe_unused]] const file_sched& arr_idx) -> RType
        {
          using a_uid_idx_type = arrow_array_uid_idx_t<Uid...>;
          const auto& [array, a_uid_idx] = std::get<a_uid_idx_type>(uid_idx);
          if (!array || !a_uid_idx) {
            SPDLOG_ERROR("access_sched: invalid arrow array or index");
            throw std::runtime_error("Invalid arrow array or index");
          }

          const auto chunk = array->chunk(0);
          if (!chunk) {
            SPDLOG_ERROR("access_sched: null chunk in arrow array");
            throw std::runtime_error("Null chunk in array");
          }

          SPDLOG_DEBUG("access_sched: chunk type='{}' length={}",
                       chunk->type()->ToString(),
                       chunk->length());

          if constexpr (std::is_integral_v<Type>
                        && sizeof(Type) >= sizeof(int32_t))
          {
            if (!is_compatible_int32_type(chunk->type_id())) {
              SPDLOG_ERROR(
                  "access_sched: type mismatch for int32 cast: "
                  "expected {} got {}",
                  ArrowTraits<Type>::Type::type_name(),
                  chunk->type()->ToString());
              throw std::runtime_error(
                  std::format("Type mismatch: expected {} got {}",
                              ArrowTraits<Type>::Type::type_name(),
                              chunk->type()->ToString()));
            }

            auto array_value = cast_to_int32_array(chunk);
            if (!array_value) {
              SPDLOG_ERROR("access_sched: failed to cast column to int32");
              throw std::runtime_error("Failed to cast column to int32");
            }

            return RType {
                access_oper(array_value, a_uid_idx, std::make_tuple(uid...)),
            };
          } else if constexpr (std::is_floating_point_v<Type>
                               && sizeof(Type) >= sizeof(double))
          {
            if (!is_compatible_double_type(chunk->type_id())) {
              SPDLOG_ERROR(
                  "access_sched: type mismatch for double cast: "
                  "expected {} got {}",
                  ArrowTraits<Type>::Type::type_name(),
                  chunk->type()->ToString());
              throw std::runtime_error(
                  std::format("Type mismatch: expected {} got {}",
                              ArrowTraits<Type>::Type::type_name(),
                              chunk->type()->ToString()));
            }

            auto array_value = cast_to_double_array(chunk);
            if (!array_value) {
              SPDLOG_ERROR("access_sched: failed to cast column to double");
              throw std::runtime_error("Failed to cast column to double");
            }

            return RType {
                access_oper(array_value, a_uid_idx, std::make_tuple(uid...)),
            };
          } else {
            if (chunk->type_id() != ArrowTraits<Type>::Type::type_id) {
              SPDLOG_ERROR(
                  "access_sched: type mismatch: expected {} got {}",
                  ArrowTraits<Type>::Type::type_name(),
                  chunk->type()->ToString());
              throw std::runtime_error(
                  std::format("Type mismatch: expected {} got {}",
                              ArrowTraits<Type>::Type::type_name(),
                              chunk->type()->ToString()));
            }

            using array_value_type = arrow::CTypeTraits<Type>::ArrayType;
            auto array_value =
                std::static_pointer_cast<array_value_type>(chunk);

            return RType {
                access_oper(
                    array_value, a_uid_idx, std::make_tuple(uid...)),
            };
          }
        },
    };

    return std::visit(visitor, sched);
  }

  template<typename Type, typename FSched, typename UidIdx, typename... Uid>
  constexpr static auto at_sched(const FSched& sched,
                                 const UidIdx& uid_idx,
                                 Uid... uid) -> Type
  {
    return access_sched<Type>(
        sched,
        uid_idx,
        [&](const auto& values, const auto& uid_idx, const auto& key) -> Type
        {
          try {
            const auto idx = uid_idx->at(key);
            const auto value = values->Value(idx);
            SPDLOG_DEBUG("at_sched: key {} idx {} value {} values {}",
                         gtopt::as_string(key),
                         idx,
                         value,
                         (void*)values.get());
            return value;
          } catch (const std::out_of_range& e) {
            SPDLOG_ERROR("Key {} not found in uid index: {}",
                         gtopt::as_string(key),
                         e.what());
            throw;
          }
        },
        uid...);
  }

  template<typename Type, typename FSched, typename UidIdx, typename... Uid>
  constexpr static auto optval_sched(const FSched& sched,
                                     const UidIdx& uid_idx,
                                     Uid... uid) -> std::optional<Type>
  {
    return access_sched<Type, std::optional<Type>>(
        sched,
        uid_idx,
        [&](const auto& values,
            const auto& uid_idx,
            const auto& key) -> std::optional<Type>
        {
          try {
            const auto idx = uid_idx->at(key);
            const auto value = values->Value(idx);
            SPDLOG_DEBUG("optval_sched: key {} idx {} value {} values {}",
                         gtopt::as_string(key),
                         idx,
                         value,
                         (void*)values.get());
            return value;

          } catch (const std::out_of_range& e) {
            SPDLOG_ERROR("Key {} not found in uid index: {}",
                         gtopt::as_string(key),
                         e.what());
            return std::nullopt;
          }
        },
        uid...);
  }
};

}  // namespace gtopt
