#pragma once

#include <gtopt/bus.hpp>
#include <gtopt/capacity.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

struct GeneratorAttrs
{
#define GTOPT_GENERATOR_ATTRS \
  SingleId bus {}; \
  OptTBRealFieldSched pmin {}; \
  OptTBRealFieldSched pmax {}; \
  OptTRealFieldSched lossfactor {}; \
  OptTRealFieldSched gcost {}; \
  GTOPT_CAPACITY_ATTRS

  GTOPT_GENERATOR_ATTRS;
};

struct Generator
{
  Uid uid {};
  Name name {};
  OptActive active {};

  GTOPT_GENERATOR_ATTRS;
};

using GeneratorVar = std::variant<Uid, Name, GeneratorAttrs>;
using OptGeneratorVar = std::optional<GeneratorVar>;

}  // namespace gtopt
