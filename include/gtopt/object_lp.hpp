/**
 * @file      object_lp.hpp
 * @brief     Linear programming wrapper for objects in the optimization model
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * Provides a wrapper class that adds linear programming capabilities to objects
 * while maintaining their original functionality. The class tracks object
 * activity status across different stages of the optimization problem.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/object_utils.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{
// Forward declarations
class InputContext;
class OutputContext;
class SystemContext;
class LinearProblem;

/**
 * @brief Wrapper class that adds LP capabilities to objects
 * @tparam ObjectType The type of object being wrapped
 *
 * This class maintains the original object while adding functionality needed
 * for linear programming, such as activity tracking across stages.
 */
template<typename ObjectType>
class ObjectLP : public ObjectUtils
{
  ObjectType m_object_;  ///< The wrapped object instance
  OptActiveSched m_active_;  ///< Schedule tracking object's active status

public:
  using object_type = ObjectType;  ///< Type of the wrapped object

  template<typename Self>
  [[nodiscard]] constexpr auto class_name(
      [[maybe_unused]] this const Self& self) noexcept
  {
    return Self::ClassName.full_name();
  }

  template<typename Self>
  [[nodiscard]] constexpr auto short_name(
      [[maybe_unused]] this const Self& self) noexcept
  {
    return Self::ClassName.short_name();
  }

  /**
   * @brief Constructs an ObjectLP by moving in an object
   * @param pobject The object to wrap and manage
   */
  template<typename OT>
    requires(!std::same_as<std::remove_cvref_t<OT>, ObjectLP>)
  explicit constexpr ObjectLP(OT&& pobject) noexcept
      : m_object_(std::forward<OT>(pobject))
      , m_active_(m_object_.active.value_or(True))
  {
  }

  template<typename OT>
  explicit constexpr ObjectLP(
      OT&& pobject,
      [[maybe_unused]] const InputContext& ic,
      [[maybe_unused]] const std::string_view& cname) noexcept
      : m_object_(std::forward<OT>(pobject))
      , m_active_(ic, cname, id(), std::move(object().active))
  {
  }

  template<typename OT>
  explicit constexpr ObjectLP(OT&& pobject,
                              [[maybe_unused]] const InputContext& ic,
                              [[maybe_unused]] const LPClassName cname) noexcept
      : m_object_(std::forward<OT>(pobject))
      , m_active_(ic, cname.full_name(), id(), std::move(object().active))
  {
  }

  /**
   * @brief Sets the object's identifier
   * @param uid Unique identifier
   * @param name Human-readable name
   * @return Reference to self for chaining
   */
  constexpr auto& set_id(Uid uid, Name name) noexcept
  {
    m_object_.uid = uid;
    m_object_.name = std::move(name);
    return *this;
  }

  /// @return The object's unique identifier
  [[nodiscard]] constexpr auto uid() const noexcept { return m_object_.uid; }

  /// @return The object's complete identifier (uid + name)
  [[nodiscard]] constexpr auto id() const noexcept
  {
    return gtopt::id(m_object_);
  }

  /**
   * @brief Checks if object is active in given stage
   * @param stage_index The stage to check
   * @return true if active in stage, false otherwise
   */
  template<typename StageLP>
  [[nodiscard]] constexpr bool is_active(const StageLP& stage) const
  {
    return m_active_.at(stage.uid()) != False;
  }

  /**
   * @brief Gets the wrapped object (explicit object syntax)
   * @tparam Self CRTP self type
   * @param self Reference to this object
   * @return Reference to the wrapped object
   */

  [[nodiscard]] constexpr auto&& object(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.m_object_);
  }
};

}  // namespace gtopt
