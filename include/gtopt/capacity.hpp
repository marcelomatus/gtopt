/**
 * @file      capacity.hpp
 * @brief     Header of
 * @date      Thu Mar 27 10:45:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

struct CapacityId
{
  using object_type = struct Capacity;
  OptUid uid;
  OptName name;

  template<typename Class = class CapacityLP>
  [[nodiscard]] constexpr auto sid() const
  {
    return ObjectSingleId<Class> {uid.has_value()
                                      ? SingleId {uid.value()}
                                      : SingleId {name.value_or(Name {})}};
  }
};

struct CapacityAttrs
{
#define GTOPT_CAPACITY_ATTRS \
  OptTRealFieldSched capacity {}; \
  OptTRealFieldSched expcap {}; \
  OptTRealFieldSched expmod {}; \
  OptTRealFieldSched capmax {}; \
  OptTRealFieldSched annual_capcost {}; \
  OptTRealFieldSched annual_derating {}

  GTOPT_CAPACITY_ATTRS;
};

struct Capacity
{
  Uid uid {};
  Name name {};
  OptActive active {};

  GTOPT_CAPACITY_ATTRS;
};

using CapacityValue = std::variant<Real, CapacityId>;
using OptCapacityValue = std::optional<CapacityValue>;

}  // namespace gtopt
