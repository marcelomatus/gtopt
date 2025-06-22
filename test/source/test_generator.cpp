#include <doctest/doctest.h>
#include <gtopt/generator.hpp>

using namespace gtopt;

TEST_CASE("Generator set_attrs functionality")
{
  Generator gen;
  GeneratorAttrs attrs;

  // Setup test values
  attrs.bus = Uid {42};
  attrs.pmin = 10.0;
  attrs.pmax = 100.0;

  // Test moving attrs to generator
  gen.set_attrs(attrs);

  CHECK(std::get<Uid>(gen.bus) == 42);
  CHECK(attrs.bus == SingleId {});  // Should be reset after exchange
  CHECK(gen.pmin.has_value());
  CHECK(gen.pmax.has_value());
  CHECK_FALSE(attrs.pmin.has_value());  // Should be moved
  CHECK_FALSE(attrs.pmax.has_value());  // Should be moved

  CHECK(std::get<Real>(gen.pmin.value()) == 10.0);
  CHECK(std::get<Real>(gen.pmax.value()) == 100.0);
}

TEST_CASE("Generator construction and attributes")
{
  Generator gen;

  // Default values
  CHECK(gen.uid == Uid {unknown_uid});
  CHECK(gen.name == Name {});
  CHECK_FALSE(gen.active.has_value());

  // Set some values
  gen.uid = Uid {1001};
  gen.name = "TestGenerator";
  gen.active = true;
  gen.bus = Uid {5};

  CHECK(gen.uid == 1001);
  CHECK(gen.name == "TestGenerator");
  CHECK(std::get<IntBool>(gen.active.value()) == 1);
  CHECK(std::get<Uid>(gen.bus) == 5);
}

TEST_CASE("GeneratorVar variant usage")
{
  GeneratorVar var_uid = Uid {123};
  GeneratorVar var_name = Name {"Gen1"};
  GeneratorAttrs attrs;
  attrs.bus = Uid {42};
  GeneratorVar var_attrs = attrs;

  // Check variant access
  CHECK(std::holds_alternative<Uid>(var_uid));
  CHECK(std::holds_alternative<Name>(var_name));
  CHECK(std::holds_alternative<GeneratorAttrs>(var_attrs));

  CHECK(std::get<Uid>(var_uid) == 123);
  CHECK(std::get<Name>(var_name) == "Gen1");
  CHECK(std::get<Uid>(std::get<GeneratorAttrs>(var_attrs).bus) == 42);
}
