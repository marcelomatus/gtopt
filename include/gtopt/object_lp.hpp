/**
 * @file      object_lp.hpp
 * @brief     Header of
 * @date      Sat May 31 23:57:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/object.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{
class InputContext;
class OutputContext;
class SystemContext;
class LinearProblem;

template<typename ObjectType>
class ObjectLP
{
  ObjectType m_object_;
  ActiveSched active;

public:
  using object_type = ObjectType;

  explicit ObjectLP(ObjectType&& pobject)
      : m_object_(std::move(pobject))
      , active(m_object_.active.value_or(True))
  {
  }

  constexpr auto& set_id(Uid uid, Name name) noexcept
  {
    m_object_.uid = uid;
    m_object_.name = std::move(name);

    return *this;
  }

  constexpr auto uid() const noexcept { return m_object_.uid; }
  constexpr auto id() const noexcept { return gtopt::id(m_object_); }

  [[nodiscard]] constexpr auto is_active(const StageIndex stage_index) const
  {
    return active.at(stage_index) != False;
  }

  template<typename Self>
  constexpr auto&& object(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_object_;
  }
};

}  // namespace gtopt
