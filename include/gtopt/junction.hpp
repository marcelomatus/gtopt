#pragma once

#include <gtopt/field_sched.hpp>

namespace gtopt
{

struct Junction
{
  Uid uid {};
  Name name {};
  OptActive active {};

  OptBool drain {};
};

}  // namespace gtopt
