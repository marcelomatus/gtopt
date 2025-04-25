/**
 * @file      element_traits.hpp
 * @brief     Header of
 * @date      Thu Apr 24 22:11:20 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <utility>

#include <gtopt/collection.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

class BusLP;

template<typename SystemContext, typename Element>
struct ElementTraits
{
  constexpr static auto&& get_elements(SystemContext& sc)
  {
    return sc.system().template elements<Element>();
  }

  template<template<typename> class Id>
  constexpr static auto get_element_index(SystemContext& sc,
                                          const Id<Element>& id)
  {
    return sc.system().element_index(id);
  }

  template<template<typename> class Id>
  constexpr static auto&& get_element(SystemContext& sc, const Id<Element>& id)
  {
    return sc.system().element(id);
  }

  template<typename ElementType2>
  constexpr static auto push_back(SystemContext& sc, ElementType2&& e)
  {
    return sc.system().template push_back<Element>(
        std::forward<ElementType2>(e));
  }
};

template<typename SystemContext>
struct ElementTraits<SystemContext, BusLP>
{
  constexpr static auto&& get_elements(SystemContext& sc)
  {
    return sc.system().template elements<BusLP>();
  }

  constexpr static auto get_element_index(SystemContext& sc,
                                          const ObjectSingleId<BusLP>& id)
  {
    return sc.get_bus_index(id);
  }

  constexpr static auto&& get_element(SystemContext& sc,
                                      const ObjectSingleId<BusLP>& id)
  {
    return sc.get_bus(id);
  }

  template<typename BusType>
  constexpr static auto&& get_element(SystemContext& sc,
                                      const ElementIndex<BusType>& id)
  {
    return sc.system().element(id);
  }

  template<typename ElementType2>
  constexpr static auto push_back(SystemContext& sc, ElementType2&& e)
  {
    return sc.system().template push_back<BusLP>(std::forward<ElementType2>(e));
  }
};

template<typename Element, typename SystemContext>
constexpr auto&& get_elements(SystemContext& sc)
{
  return ElementTraits<SystemContext, Element>::get_elements(sc);
}

template<typename Element, typename SystemContext, template<typename> class Id>
constexpr auto get_element_index(SystemContext& sc, const Id<Element>& id)
{
  return ElementTraits<SystemContext, Element>::get_element_index(sc, id);
}

template<typename Element, typename SystemContext, template<typename> class Id>
constexpr auto&& get_element(SystemContext& sc, const Id<Element>& id)
{
  return ElementTraits<SystemContext, Element>::get_element(sc, id);
}

template<typename Element, typename SystemContext>
constexpr auto push_back(SystemContext& sc, Element&& e)
{
  return ElementTraits<SystemContext, Element>::push_back(
      sc, std::forward<Element>(e));
}

}  // namespace gtopt
