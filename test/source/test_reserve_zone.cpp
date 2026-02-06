#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/reserve_zone.hpp>
#include <gtopt/stage.hpp>

using namespace gtopt;

TEST_CASE("ReserveZone construction and default values")
{
  const ReserveZone reserve_zone;

  // Check default values
  CHECK(reserve_zone.uid == Uid {unknown_uid});
  CHECK(reserve_zone.name == Name {});
  CHECK_FALSE(reserve_zone.active.has_value());

  // Check default values for reserve requirements
  CHECK_FALSE(reserve_zone.urreq.has_value());  // up reserve requirement
  CHECK_FALSE(reserve_zone.drreq.has_value());  // down reserve requirement
  CHECK_FALSE(reserve_zone.urcost.has_value());  // up reserve shortage cost
  CHECK_FALSE(reserve_zone.drcost.has_value());  // down reserve shortage cost
}

TEST_CASE("ReserveZone attribute assignment")
{
  ReserveZone reserve_zone;

  // Assign identification values
  reserve_zone.uid = 2001;
  reserve_zone.name = "TestReserveZone";
  reserve_zone.active = true;

  // Assign reserve requirements and costs
  reserve_zone.urreq = 50.0;  // 50 MW up reserve requirement
  reserve_zone.drreq = 30.0;  // 30 MW down reserve requirement
  reserve_zone.urcost = 1000.0;  // $1000/MW shortage cost for up reserve
  reserve_zone.drcost = 800.0;  // $800/MW shortage cost for down reserve

  // Check assigned values
  CHECK(reserve_zone.uid == 2001);
  CHECK(reserve_zone.name == "TestReserveZone");
  CHECK(std::get<IntBool>(reserve_zone.active.value()) == 1);

  // For OptTBRealFieldSched and OptTRealFieldSched types, we need to get the
  // Real variant alternative Check urreq (TB = Time Block schedule)
  REQUIRE(reserve_zone.urreq.has_value());
  auto* urreq_real_ptr = std::get_if<Real>(&reserve_zone.urreq.value());
  REQUIRE(urreq_real_ptr != nullptr);
  CHECK(*urreq_real_ptr == 50.0);

  // Check drreq
  REQUIRE(reserve_zone.drreq.has_value());
  auto* drreq_real_ptr = std::get_if<Real>(&reserve_zone.drreq.value());
  REQUIRE(drreq_real_ptr != nullptr);
  CHECK(*drreq_real_ptr == 30.0);

  // Check urcost (T = Time schedule, not by block)
  REQUIRE(reserve_zone.urcost.has_value());
  auto* urcost_real_ptr = std::get_if<Real>(&reserve_zone.urcost.value());
  REQUIRE(urcost_real_ptr != nullptr);
  CHECK(*urcost_real_ptr == 1000.0);

  // Check drcost
  REQUIRE(reserve_zone.drcost.has_value());
  auto* drcost_real_ptr = std::get_if<Real>(&reserve_zone.drcost.value());
  REQUIRE(drcost_real_ptr != nullptr);
  CHECK(*drcost_real_ptr == 800.0);
}

TEST_CASE("ReserveZone with time-varying costs")
{
  ReserveZone reserve_zone;

  // Create vectors for time-varying costs
  std::vector<Real> urcost_schedule = {900.0, 1000.0, 1200.0, 950.0};
  std::vector<Real> drcost_schedule = {700.0, 800.0, 850.0, 750.0};

  // Assign to reserve zone
  reserve_zone.urcost = urcost_schedule;
  reserve_zone.drcost = drcost_schedule;

  // Verify costs were assigned properly
  REQUIRE(reserve_zone.urcost.has_value());
  REQUIRE(reserve_zone.drcost.has_value());

  // Check that we have vectors, not scalars
  auto* urcost_vec_ptr =
      std::get_if<std::vector<Real>>(&reserve_zone.urcost.value());
  auto* drcost_vec_ptr =
      std::get_if<std::vector<Real>>(&reserve_zone.drcost.value());

  REQUIRE(urcost_vec_ptr != nullptr);
  REQUIRE(drcost_vec_ptr != nullptr);

  // Check the schedule values
  CHECK(urcost_vec_ptr->size() == 4);
  CHECK(drcost_vec_ptr->size() == 4);

  CHECK((*urcost_vec_ptr)[0] == 900.0);
  CHECK((*urcost_vec_ptr)[1] == 1000.0);
  CHECK((*urcost_vec_ptr)[2] == 1200.0);
  CHECK((*urcost_vec_ptr)[3] == 950.0);

  CHECK((*drcost_vec_ptr)[0] == 700.0);
  CHECK((*drcost_vec_ptr)[1] == 800.0);
  CHECK((*drcost_vec_ptr)[2] == 850.0);
  CHECK((*drcost_vec_ptr)[3] == 750.0);
}
