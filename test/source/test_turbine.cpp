#include <doctest/doctest.h>
#include <gtopt/turbine.hpp>

using namespace gtopt;

TEST_CASE("Turbine construction and default values")
{
  const Turbine turbine;

  CHECK(turbine.uid == Uid {unknown_uid});
  CHECK(turbine.name == Name {});
  CHECK_FALSE(turbine.active.has_value());

  CHECK(turbine.waterway == SingleId {unknown_uid});
  CHECK(turbine.generator == SingleId {unknown_uid});

  CHECK_FALSE(turbine.drain.has_value());
  CHECK_FALSE(turbine.conversion_rate.has_value());
  CHECK_FALSE(turbine.capacity.has_value());
}

TEST_CASE("Turbine attribute assignment")
{
  Turbine turbine;

  turbine.uid = 5001;
  turbine.name = "TestTurbine";
  turbine.active = true;

  turbine.waterway = Uid {1001};
  turbine.generator = Uid {2001};

  turbine.drain = true;
  turbine.conversion_rate = 0.85;
  turbine.capacity = 300.0;

  CHECK(turbine.uid == 5001);
  CHECK(turbine.name == "TestTurbine");
  CHECK(std::get<IntBool>(turbine.active.value()) == 1);

  CHECK(std::get<Uid>(turbine.waterway) == Uid {1001});
  CHECK(std::get<Uid>(turbine.generator) == Uid {2001});

  REQUIRE(turbine.drain.has_value());
  CHECK(turbine.drain.value() == true);

  REQUIRE(turbine.conversion_rate.has_value());
  CHECK(*std::get_if<Real>(&turbine.conversion_rate.value()) == 0.85);

  REQUIRE(turbine.capacity.has_value());
  CHECK(*std::get_if<Real>(&turbine.capacity.value()) == 300.0);
}

TEST_CASE("Turbine with time-varying conversion rate")
{
  Turbine turbine;

  std::vector<Real> rate_schedule = {0.80, 0.85, 0.90, 0.82};
  turbine.conversion_rate = rate_schedule;

  REQUIRE(turbine.conversion_rate.has_value());
  auto* vec_ptr =
      std::get_if<std::vector<Real>>(&turbine.conversion_rate.value());
  REQUIRE(vec_ptr != nullptr);
  CHECK(vec_ptr->size() == 4);
  CHECK((*vec_ptr)[0] == 0.80);
  CHECK((*vec_ptr)[1] == 0.85);
  CHECK((*vec_ptr)[2] == 0.90);
  CHECK((*vec_ptr)[3] == 0.82);
}
