/**
 * @file      demand.hpp
 * @brief     Header of
 * @date      Thu Mar 27 09:12:21 2025
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
struct DemandAttrs
{
  SingleId bus {};
  OptTBRealFieldSched lmax {};
  OptTRealFieldSched lossfactor {};
  OptTRealFieldSched fcost {};
  OptTRealFieldSched emin {};
  OptTRealFieldSched ecost {};

  OptTRealFieldSched capacity {};
  OptTRealFieldSched expcap {};
  OptTRealFieldSched expmod {};
  OptTRealFieldSched capmax {};
  OptTRealFieldSched annual_capcost {};
  OptTRealFieldSched annual_derating {};
};

struct Demand : DemandAttrs
{
  Uid uid {};
  Name name {};
  OptActive active {};

  [[nodiscard]] auto id() const -> Id { return {uid, name}; }
};

using DemandVar = std::variant<Uid, Name, DemandAttrs>;
using OptDemandVar = std::optional<DemandVar>;

}  // namespace gtopt
