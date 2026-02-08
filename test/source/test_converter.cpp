#include <doctest/doctest.h>
#include <gtopt/converter.hpp>

using namespace gtopt;

TEST_CASE("Converter construction and default values")
{
  const Converter converter;

  CHECK(converter.uid == Uid {unknown_uid});
  CHECK(converter.name == Name {});
  CHECK_FALSE(converter.active.has_value());

  CHECK(converter.battery == SingleId {unknown_uid});
  CHECK(converter.generator == SingleId {unknown_uid});
  CHECK(converter.demand == SingleId {unknown_uid});

  CHECK_FALSE(converter.conversion_rate.has_value());
  CHECK_FALSE(converter.capacity.has_value());
  CHECK_FALSE(converter.expcap.has_value());
  CHECK_FALSE(converter.expmod.has_value());
  CHECK_FALSE(converter.capmax.has_value());
  CHECK_FALSE(converter.annual_capcost.has_value());
  CHECK_FALSE(converter.annual_derating.has_value());
}

TEST_CASE("Converter attribute assignment")
{
  Converter converter;

  converter.uid = 4001;
  converter.name = "TestConverter";
  converter.active = true;

  converter.battery = Uid {1001};
  converter.generator = Uid {2001};
  converter.demand = Uid {3001};

  converter.conversion_rate = 0.95;
  converter.capacity = 500.0;
  converter.expcap = 100.0;
  converter.expmod = 25.0;
  converter.capmax = 1000.0;
  converter.annual_capcost = 20000.0;
  converter.annual_derating = 0.01;

  CHECK(converter.uid == 4001);
  CHECK(converter.name == "TestConverter");
  CHECK(std::get<IntBool>(converter.active.value()) == 1);

  CHECK(std::get<Uid>(converter.battery) == Uid {1001});
  CHECK(std::get<Uid>(converter.generator) == Uid {2001});
  CHECK(std::get<Uid>(converter.demand) == Uid {3001});

  REQUIRE(converter.conversion_rate.has_value());
  CHECK(*std::get_if<Real>(&converter.conversion_rate.value()) == 0.95);

  CHECK(*std::get_if<Real>(&converter.capacity.value()) == 500.0);
  CHECK(*std::get_if<Real>(&converter.expcap.value()) == 100.0);
  CHECK(*std::get_if<Real>(&converter.expmod.value()) == 25.0);
  CHECK(*std::get_if<Real>(&converter.capmax.value()) == 1000.0);
  CHECK(*std::get_if<Real>(&converter.annual_capcost.value()) == 20000.0);
  CHECK(*std::get_if<Real>(&converter.annual_derating.value()) == 0.01);
}

TEST_CASE("Converter with inactive status")
{
  Converter converter;

  converter.uid = 4002;
  converter.name = "InactiveConverter";
  converter.active = false;

  CHECK(converter.uid == 4002);
  CHECK(converter.name == "InactiveConverter");
  REQUIRE(converter.active.has_value());
  CHECK(std::get<IntBool>(converter.active.value()) == 0);
}
