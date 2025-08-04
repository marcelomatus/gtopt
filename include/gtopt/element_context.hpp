/**
 * @file      element_context.hpp
 * @brief     Provides a context for interacting with elements within a SystemLP.
 * @date      Tue Apr 22 22:52:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ElementContext class, which serves as an interface for
 * managing and accessing elements in a linear programming model of a system.
 * It simplifies element creation, retrieval, and indexing by forwarding calls
 * to the underlying SystemLP type.
 */
#pragma once

#include <functional>
#include <variant>

#include <gtopt/basic_types.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/overload.hpp>

namespace gtopt
{

/**
 * @brief Provides a context for interacting with elements within a SystemLP.
 *
 * This class acts as a facade over a `SystemLP`-like object, providing a
 * simplified interface for element management. It forwards calls for element
 * access, indexing, and creation to the underlying system model.
 *
 * @tparam SystemLP_Type The type of the system linear programming model.
 */
template<typename SystemLP_Type>
class ElementContext
{
public:
  /**
   * @brief Constructs an ElementContext.
   * @param system_lp A reference to the system LP object.
   */
  explicit constexpr ElementContext(SystemLP_Type& system_lp) noexcept
      : m_system_lp_(system_lp)
  {
  }

  /**
   * @brief Gets the underlying SystemLP object.
   * @return A reference to the SystemLP object, preserving const and value
   * category.
   */
  [[nodiscard]] constexpr auto&& system_lp(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.m_system_lp_.get());
  }

  /**
   * @brief Gets a container of all elements of a specific type.
   * @tparam Element The type of elements to retrieve.
   * @return A const reference to the container of elements.
   */
  template<typename Element>
  [[nodiscard]] constexpr auto&& elements() const noexcept
  {
    return system_lp().template elements<Element>();
  }

  /**
   * @brief Gets the index of an element by its ID.
   * @tparam Element The type of the element.
   * @tparam Id The ID type wrapper (e.g., ObjectId).
   * @param id The ID of the element.
   * @return The ElementIndex for the given ID.
   * @throws std::out_of_range if the element is not found.
   */
  template<typename Element, template<typename> class Id>
  [[nodiscard]] constexpr auto element_index(const Id<Element>& id) const
  {
    return system_lp().element_index(id);
  }

  /**
   * @brief Gets a reference to an element by its ID.
   * @tparam Element The type of the element.
   * @tparam Id The ID type wrapper (e.g., ObjectId).
   * @param id The ID of the element.
   * @return A reference to the element.
   * @throws std::out_of_range if the element is not found.
   */
  template<typename Element, template<typename> class Id>
  [[nodiscard]] constexpr auto&& element(const Id<Element>& id) const
  {
    return system_lp().element(id);
  }

  /**
   * @brief Adds a new element to the system.
   * @tparam Element The type of the element.
   * @param element The element to add (rvalue reference).
   * @return The ElementIndex of the newly added element.
   */
  template<typename Element>
  [[nodiscard]] auto add_element(Element&& element)
  {
    return system_lp().template push_back<Element>(
        std::forward<Element>(element));
  }

  /**
   * @brief Finds an existing element's index or creates a new one.
   *
   * This function attempts to find an element by Uid or Name. If not found,
   * it creates a new element using the provided attributes and original object
   * data.
   *
   * @tparam Element The type of the element to find or create.
   * @tparam Self The type of this ElementContext object.
   * @tparam Object The type of the original object containing base data (uid,
   * name, etc.).
   * @tparam Attrs The type of the attributes for creating a new element.
   * @param self The ElementContext instance.
   * @param objori The original object with base data.
   * @param element_var A variant holding either a Uid, a Name, or Attrs.
   * @return ElementIndex of the found or created element.
   */
  template<typename Element, typename Self, typename Object, typename Attrs>
  [[nodiscard]] auto
  make_element_index(this Self&& self,
                     const Object& objori,
                     const std::variant<Uid, Name, Attrs>& element_var)
      -> ElementIndex<Element>
  {
    using ElementId = ObjectSingleId<Element>;
    using ObjElement = typename Element::object_type;
    return std::visit(
        Overload {[&](const Uid uid)
                  { return self.element_index(ElementId {uid}); },
                  [&](const Name& name)
                  { return self.element_index(ElementId {name}); },
                  [&](Attrs attrs)
                  {
                    ObjElement objele {.uid = objori.uid,
                                       .name = objori.name,
                                       .active = objori.active};
                    return self.add_element(
                        Element {objele.set_attrs(std::move(attrs)),
                                 std::forward<Self>(self)});
                  }},
        element_var);
  }

private:
  std::reference_wrapper<SystemLP_Type> m_system_lp_;
};

}  // namespace gtopt
