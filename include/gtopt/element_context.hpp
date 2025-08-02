/**
 * @file      element_index.hpp
 * @brief     Header of
 * @date      Tue Apr 22 22:52:04 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
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

template<typename SystemLP_Type>
class ElementContext
{
public:
  explicit ElementContext(SystemLP_Type& system_lp)
      : m_system_lp_(system_lp)
  {
  }

  template<typename Self>
  [[nodiscard]] constexpr auto&& system_lp(this Self&& self)
  {
    return std::forward_like<Self>(self.m_system_lp_.get());
  }

  template<typename Element>
  auto&& elements() const
  {
    return system_lp().template elements<Element>();
  }

  template<typename Element, template<typename> class Id>
  auto element_index(const Id<Element>& id) const
  {
    return system_lp().element_index(id);
  }

  template<typename Element, template<typename> class Id>
  auto&& element(const Id<Element>& id) const
  {
    return system_lp().element(id);
  }

  template<typename Element>
  auto add_element(Element&& element)
  {
    return system_lp().template push_back<Element>(
        std::forward<Element>(element));
  }

  template<typename Element, typename Self, typename Object, typename Attrs>
  auto make_element_index(this Self&& self,
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
