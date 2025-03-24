/**
 * @file      single_id.hpp
 * @brief     Header of
 * @date      Sun Mar 23 10:46:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

using SingleId = std::variant<Uid, Name>;
using OptSingleId = std::optional<SingleId>;

template<typename Object>
struct ObjectSingleId : SingleId
{
  using object_type = Object;
  using SingleId::SingleId;

  ObjectSingleId(ObjectSingleId&&) noexcept = default;
  ObjectSingleId(const ObjectSingleId&) = default;
  ObjectSingleId() = default;
  ObjectSingleId& operator=(ObjectSingleId&&) noexcept = default;
  ObjectSingleId& operator=(const ObjectSingleId&) noexcept = default;
  ~ObjectSingleId() = default;

  explicit ObjectSingleId(const SingleId& id)
      : SingleId(id)
  {
  }

  explicit ObjectSingleId(SingleId&& id) noexcept
      : SingleId(std::move(id))
  {
  }

  explicit ObjectSingleId(Name id) noexcept
      : SingleId(std::move(id))
  {
  }

  explicit ObjectSingleId(Uid id) noexcept
      : SingleId(id)
  {
  }

  constexpr auto uid() const { return std::get<Uid>(*this); }
  constexpr const auto& name() const { return std::get<Name>(*this); }
};

}  // namespace gtopt
