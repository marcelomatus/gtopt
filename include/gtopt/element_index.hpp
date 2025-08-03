/**
 * @file      element_index.hpp
 * @brief     Type-safe strong index type for elements
 * @date      Fri Apr 25 00:33:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a type-safe strongly typed index for elements
 * to prevent accidental access with incorrect index types.
 */

#pragma once

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/**
 * A strongly-typed index for accessing elements in a collection.
 * Inherits from StrongIndexType to enforce type safety in element access.
 *
 * @tparam Element The element type this index is associated with
 */
template<typename Element>
struct ElementIndex : StrongIndexType<Element>
{
  using Base = StrongIndexType<Element>;
  using Base::Base;

  constexpr static auto Unknown = unknown_index;

  ElementIndex()
      : Base(Unknown)
  {
  }
};

}  // namespace gtopt
