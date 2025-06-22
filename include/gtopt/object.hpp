/**
 * @file      object.hpp
 * @brief     Core object types and utilities for the optimization framework
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * Defines fundamental object types and identification utilities used throughout
 * the optimization framework. Provides base functionality for uniquely
 * identifying and tracking objects in the system.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/object_utils.hpp>  // Include the new ObjectUtils
#include <gtopt/single_id.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

/**
 * @brief Basic attributes common to all objects in the system
 *
 * Contains the minimal set of attributes needed to uniquely identify
 * and track an object's state in the optimization framework.
 */
struct ObjectAttrs
{
  Uid uid {unknown_uid};  ///< Unique identifier for the object
  Name name {};  ///< Human-readable name of the object
  OptActive active {};  ///< Optional activity status of the object
};

/**
 * @brief Creates an Id from an object's attributes
 * @tparam Obj The object type (must have uid and name members)
 * @param obj The object to get identification from
 * @return Id containing the object's uid and name
 */
template<typename Obj>
[[nodiscard]] constexpr auto id(const Obj& obj) noexcept -> Id
{
  return {obj.uid, obj.name};
}

/**
 * @brief Base object type providing common identification functionality
 *
 * Serves as the foundation for all objects in the optimization framework.
 * Provides consistent identification behavior through the id() method.
 */
struct Object : public ObjectUtils
{
  /**
   * @brief Gets the object's identifier (explicit object syntax)
   * @tparam Self CRTP self type
   * @param self Reference to this object
   * @return Id containing the object's uid and name
   */
  template<typename Self>
  [[nodiscard]] constexpr auto id(this const Self& self) noexcept
  {
    return gtopt::id(self);
  }

  template<typename Self>
  [[nodiscard]] constexpr auto class_name(
      [[maybe_unused]] this const Self& self) noexcept
  {
    return Self::ClassName;
  }

};

}  // namespace gtopt
