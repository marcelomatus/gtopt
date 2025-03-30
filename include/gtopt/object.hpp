/**
 * @file      object.hpp
 * @brief     Header of
 * @date      Sat Mar 29 11:56:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

struct ObjectAttrs
{
#define GTOPT_OBJECT_ATTRS \
  Uid uid {}; \
  Name name {}; \
  OptActive active {}

  GTOPT_OBJECT_ATTRS;
};

template<typename Obj>
constexpr auto id(const Obj& obj) -> Id
{
  return {obj.uid, obj.name};
}

struct Object
{
  template<class Self>
  constexpr auto id(this const Self& self)
  {
    return gtopt::id(self);
  }
};

}  // namespace gtopt
