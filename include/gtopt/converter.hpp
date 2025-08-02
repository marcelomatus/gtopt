#pragma once

#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

struct Converter
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  SingleId battery {unknown_uid};
  SingleId generator {unknown_uid};
  SingleId demand {unknown_uid};

  OptTRealFieldSched conversion_rate {};

  OptTRealFieldSched capacity {};
  OptTRealFieldSched expcap {};
  OptTRealFieldSched expmod {};
  OptTRealFieldSched capmax {};
  OptTRealFieldSched annual_capcost {};
  OptTRealFieldSched annual_derating {};
};

}  // namespace gtopt
