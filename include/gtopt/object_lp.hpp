#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/input_context.hpp>
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

  explicit ObjectLP(const InputContext& /*ic*/,
                    const std::string_view& /*ClassName*/,
                    ObjectType&& pobject)
      : m_object_(std::move(pobject))
      , active(m_object_.active.value_or(True))
  {
  }

  auto& set_id(Uid uid, const Name& name)
  {
    m_object_.uid = uid;
    m_object_.name = name;
    return *this;
  }

  constexpr auto uid() const { return m_object_.uid; }
  constexpr auto id() const { return gtopt::id(m_object_); }

  constexpr auto is_active(const StageIndex stage_index) const
  {
    return active.at(stage_index) != False;
  }

  constexpr auto&& object() { return m_object_; }
  constexpr auto&& object() const { return m_object_; }
};

}  // namespace gtopt
