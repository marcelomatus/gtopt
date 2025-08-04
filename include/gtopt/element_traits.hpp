/**
 * @file      element_traits.hpp
 * @brief     Defines traits for accessing and manipulating elements in a system
 *            context.
 * @date      Thu Apr 24 22:11:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a customization point `ElementTraits` that allows
 * different element types to be handled uniformly. It offers a default
 * implementation for standard elements and a specialization for `BusLP` which
 * requires custom logic. Free functions are provided to simplify trait usage.
 */

#pragma once

#include <utility>

#include <gtopt/element_index.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

class BusLP;

/**
 * @brief Defines default traits for element access and manipulation.
 *
 * This traits struct provides a generic implementation for interacting with
 * elements within a `SystemContext`. It forwards calls to the underlying
 * `SystemLP` object.
 *
 * @tparam SystemContext The context providing access to the system model.
 * @tparam Element The type of element these traits are for.
 */
template<typename SystemContext, typename Element>
struct ElementTraits
{
  /**
   * @brief Gets a container of all elements of a specific type.
   * @param sc The system context.
   * @return A reference to the container of elements.
   */
  [[nodiscard]] constexpr static auto&& get_elements(SystemContext& sc) noexcept
  {
    return sc.system().template elements<Element>();
  }

  /**
   * @brief Gets the index of an element by its ID.
   * @tparam Id The ID type wrapper.
   * @param sc The system context.
   * @param id The ID of the element.
   * @return The `ElementIndex` for the given ID.
   */
  template<template<typename> class Id>
  [[nodiscard]] constexpr static auto get_element_index(SystemContext& sc,
                                                        const Id<Element>& id)
  {
    return sc.system().element_index(id);
  }

  /**
   * @brief Gets a reference to an element by its ID.
   * @tparam Id The ID type wrapper.
   * @param sc The system context.
   * @param id The ID of the element.
   * @return A reference to the element.
   */
  template<template<typename> class Id>
  [[nodiscard]] constexpr static auto&& get_element(SystemContext& sc,
                                                    const Id<Element>& id)
  {
    return sc.system().element(id);
  }

  /**
   * @brief Adds a new element to the system.
   * @tparam ElementType2 The type of the element to add.
   * @param sc The system context.
   * @param e The element to add.
   * @return The `ElementIndex` of the newly added element.
   */
  template<typename ElementType2>
  [[nodiscard]] constexpr static auto push_back(SystemContext& sc,
                                                ElementType2&& e)
  {
    return sc.system().template push_back<Element>(
        std::forward<ElementType2>(e));
  }
};

/**
 * @brief Specialization of `ElementTraits` for `BusLP` elements.
 *
 * This specialization handles the unique way `BusLP` elements are accessed,
 * such as using dedicated `get_bus_index` and `get_bus` methods on the
 * `SystemContext`.
 *
 * @tparam SystemContext The context providing access to the system model.
 */
template<typename SystemContext>
struct ElementTraits<SystemContext, BusLP>
{
  /**
   * @brief Gets a container of all `BusLP` elements.
   * @param sc The system context.
   * @return A reference to the container of `BusLP` elements.
   */
  [[nodiscard]] constexpr static auto&& get_elements(SystemContext& sc) noexcept
  {
    return sc.system().template elements<BusLP>();
  }

  /**
   * @brief Gets the index of a `BusLP` element by its ID.
   * @param sc The system context.
   * @param id The single ID of the `BusLP` element.
   * @return The `ElementIndex` for the given ID.
   */
  [[nodiscard]] constexpr static auto get_element_index(
      SystemContext& sc, const ObjectSingleId<BusLP>& id)
  {
    return sc.get_bus_index(id);
  }

  /**
   * @brief Gets a reference to a `BusLP` element by its single ID.
   * @param sc The system context.
   * @param id The single ID of the `BusLP` element.
   * @return A reference to the `BusLP` element.
   */
  [[nodiscard]] constexpr static auto&&
  get_element(SystemContext& sc, const ObjectSingleId<BusLP>& id)
  {
    return sc.get_bus(id);
  }

  /**
   * @brief Gets a reference to a bus element by its `ElementIndex`.
   * @tparam BusType The bus type (e.g., `BusLP`).
   * @param sc The system context.
   * @param id The index of the bus element.
   * @return A reference to the bus element.
   */
  template<typename BusType>
  [[nodiscard]] constexpr static auto&&
  get_element(SystemContext& sc, const ElementIndex<BusType>& id)
  {
    return sc.system().element(id);
  }

  /**
   * @brief Adds a new `BusLP` element to the system.
   * @tparam ElementType2 The type of the element to add.
   * @param sc The system context.
   * @param e The `BusLP` element to add.
   * @return The `ElementIndex` of the newly added element.
   */
  template<typename ElementType2>
  [[nodiscard]] constexpr static auto push_back(SystemContext& sc,
                                                ElementType2&& e)
  {
    return sc.system().template push_back<BusLP>(std::forward<ElementType2>(e));
  }
};

/**
 * @brief Free function to get a container of all elements of a specific type.
 * @tparam Element The type of elements to retrieve.
 * @tparam SystemContext The context providing access to the system model.
 * @param sc The system context.
 * @return A reference to the container of elements.
 */
template<typename Element, typename SystemContext>
[[nodiscard]] constexpr auto&& get_elements(SystemContext& sc) noexcept
{
  return ElementTraits<SystemContext, Element>::get_elements(sc);
}

/**
 * @brief Free function to get the index of an element by its ID.
 * @tparam Element The type of the element.
 * @tparam SystemContext The context providing access to the system model.
 * @tparam Id The ID type wrapper.
 * @param sc The system context.
 * @param id The ID of the element.
 * @return The `ElementIndex` for the given ID.
 */
template<typename Element, typename SystemContext, template<typename> class Id>
[[nodiscard]] constexpr auto get_element_index(SystemContext& sc,
                                               const Id<Element>& id)
{
  return ElementTraits<SystemContext, Element>::get_element_index(sc, id);
}

/**
 * @brief Free function to get a reference to an element by its ID.
 * @tparam Element The type of the element.
 * @tparam SystemContext The context providing access to the system model.
 * @tparam Id The ID type wrapper.
 * @param sc The system context.
 * @param id The ID of the element.
 * @return A reference to the element.
 */
template<typename Element, typename SystemContext, template<typename> class Id>
[[nodiscard]] constexpr auto&& get_element(SystemContext& sc,
                                           const Id<Element>& id)
{
  return ElementTraits<SystemContext, Element>::get_element(sc, id);
}

/**
 * @brief Free function to add a new element to the system.
 * @tparam Element The type of element to create.
 * @tparam SystemContext The context providing access to the system model.
 * @tparam E The type of the element instance to add.
 * @param sc The system context.
 * @param element The element to add.
 * @return The `ElementIndex` of the newly added element.
 */
template<typename Element, typename SystemContext, typename E>
[[nodiscard]] auto push_back(SystemContext& sc, E&& element)
{
  return ElementTraits<SystemContext, Element>::push_back(
      sc, std::forward<E>(element));
}

}  // namespace gtopt
