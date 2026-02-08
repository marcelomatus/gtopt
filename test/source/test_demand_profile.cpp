#include <doctest/doctest.h>
#include <gtopt/demand_profile.hpp>

using namespace gtopt;

TEST_CASE("DemandProfile construction and default values")
{
  const DemandProfile demand_profile;

  CHECK(demand_profile.uid == Uid {unknown_uid});
  CHECK(demand_profile.name == Name {});
  CHECK_FALSE(demand_profile.active.has_value());

  CHECK(demand_profile.demand == SingleId {unknown_uid});
  CHECK_FALSE(demand_profile.scost.has_value());
}

TEST_CASE("DemandProfile attribute assignment")
{
  DemandProfile demand_profile;

  demand_profile.uid = 10001;
  demand_profile.name = "TestDemandProfile";
  demand_profile.active = true;

  demand_profile.demand = Uid {3001};
  demand_profile.scost = 50.0;

  CHECK(demand_profile.uid == 10001);
  CHECK(demand_profile.name == "TestDemandProfile");
  CHECK(std::get<IntBool>(demand_profile.active.value()) == 1);

  CHECK(std::get<Uid>(demand_profile.demand) == Uid {3001});

  REQUIRE(demand_profile.scost.has_value());
  CHECK(*std::get_if<Real>(&demand_profile.scost.value())
        == doctest::Approx(50.0));
}

TEST_CASE("DemandProfile with inactive status")
{
  DemandProfile demand_profile;

  demand_profile.uid = 10002;
  demand_profile.name = "InactiveDemandProfile";
  demand_profile.active = false;

  CHECK(demand_profile.uid == 10002);
  REQUIRE(demand_profile.active.has_value());
  CHECK(std::get<IntBool>(demand_profile.active.value()) == 0);
}
