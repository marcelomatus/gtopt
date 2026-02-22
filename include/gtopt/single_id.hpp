/**
 * @file      single_id.hpp
 * @brief     Single-value identifier types (UID or name) for component lookup
 * @date      Sun Mar 23 10:46:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides `SingleId` (a `std::variant<Uid, Name>`) and the
 * `ObjectSingleId` wrapper used to reference system components either by
 * their numeric UID or their string name in JSON/Parquet input files.
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{
template<typename Obj>
constexpr auto get_uid(const Obj& obj) -> Uid
{
  return obj.uid();
}

template<typename Obj>
constexpr auto get_name(const Obj& obj)
{
  return obj.name();
}

template<>
constexpr auto get_uid<Id>(const Id& obj) -> Uid
{
  return obj.first;
}

template<>
constexpr auto get_name<Id>(const Id& obj)
{
  return obj.second;
}

template<typename Object>
struct ObjectId : Id
{
  constexpr explicit ObjectId(const Id& id)
      : Id(id)
  {
  }
};

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

  [[nodiscard]] constexpr auto uid() const { return std::get<Uid>(*this); }
  [[nodiscard]] constexpr const auto& name() const
  {
    return std::get<Name>(*this);
  }
};

}  // namespace gtopt
