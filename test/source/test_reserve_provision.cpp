#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/reserve_provision.hpp>
#include <gtopt/stage.hpp>

using namespace gtopt;

TEST_CASE("ReserveProvision construction and default values")
{
  ReserveProvision reserve_provision;

  // Check default values for identification
  CHECK(reserve_provision.uid == Uid {unknown_uid});
  CHECK(reserve_provision.name == Name {});
  CHECK_FALSE(reserve_provision.active.has_value());

  // Check default values for references
  CHECK(reserve_provision.generator == SingleId {unknown_uid});
  CHECK(reserve_provision.reserve_zones == String {});

  // Check default values for provision limits
  CHECK_FALSE(reserve_provision.urmax.has_value());  // up reserve max
  CHECK_FALSE(reserve_provision.drmax.has_value());  // down reserve max

  // Check default values for capacity factors
  CHECK_FALSE(reserve_provision.ur_capacity_factor.has_value());
  CHECK_FALSE(reserve_provision.dr_capacity_factor.has_value());

  // Check default values for provision factors
  CHECK_FALSE(reserve_provision.ur_provision_factor.has_value());
  CHECK_FALSE(reserve_provision.dr_provision_factor.has_value());

  // Check default values for costs
  CHECK_FALSE(reserve_provision.urcost.has_value());
  CHECK_FALSE(reserve_provision.drcost.has_value());
}

TEST_CASE("ReserveProvision attribute assignment")
{
  ReserveProvision reserve_provision;

  // Assign identification values
  reserve_provision.uid = 3001;
  reserve_provision.name = "TestReserveProvision";
  reserve_provision.active = true;

  // Assign references
  reserve_provision.generator = Uid {1001};  // Reference to a generator UID
  reserve_provision.reserve_zones =
      "2001:2002";  // References to reserve zone UIDs, colon-separated

  // Assign max values
  reserve_provision.urmax = 50.0;  // 50 MW up reserve max
  reserve_provision.drmax = 30.0;  // 30 MW down reserve max

  // Assign capacity factors (how much of capacity can provide reserves)
  reserve_provision.ur_capacity_factor =
      0.8;  // 80% of capacity can provide up reserves
  reserve_provision.dr_capacity_factor =
      0.6;  // 60% of capacity can provide down reserves

  // Assign provision factors (contribution to reserve requirements)
  reserve_provision.ur_provision_factor =
      1.0;  // 100% contribution factor for up reserves
  reserve_provision.dr_provision_factor =
      1.0;  // 100% contribution factor for down reserves

  // Assign cost values
  reserve_provision.urcost = 10.0;  // $10/MW for up reserve
  reserve_provision.drcost = 8.0;  // $8/MW for down reserve

  // Check assigned values
  CHECK(reserve_provision.uid == 3001);
  CHECK(reserve_provision.name == "TestReserveProvision");
  CHECK(std::get<IntBool>(reserve_provision.active.value()) == 1);

  // Check references
  CHECK(std::get<Uid>(reserve_provision.generator) == Uid {1001});
  CHECK(reserve_provision.reserve_zones == "2001:2002");

  // Check max values
  REQUIRE(reserve_provision.urmax.has_value());
  auto* urmax_real_ptr = std::get_if<Real>(&reserve_provision.urmax.value());
  REQUIRE(urmax_real_ptr != nullptr);
  CHECK(*urmax_real_ptr == 50.0);

  REQUIRE(reserve_provision.drmax.has_value());
  auto* drmax_real_ptr = std::get_if<Real>(&reserve_provision.drmax.value());
  REQUIRE(drmax_real_ptr != nullptr);
  CHECK(*drmax_real_ptr == 30.0);

  // Check capacity factors
  REQUIRE(reserve_provision.ur_capacity_factor.has_value());
  auto* ur_capacity_factor_ptr =
      std::get_if<Real>(&reserve_provision.ur_capacity_factor.value());
  REQUIRE(ur_capacity_factor_ptr != nullptr);
  CHECK(*ur_capacity_factor_ptr == 0.8);

  REQUIRE(reserve_provision.dr_capacity_factor.has_value());
  auto* dr_capacity_factor_ptr =
      std::get_if<Real>(&reserve_provision.dr_capacity_factor.value());
  REQUIRE(dr_capacity_factor_ptr != nullptr);
  CHECK(*dr_capacity_factor_ptr == 0.6);

  // Check provision factors
  REQUIRE(reserve_provision.ur_provision_factor.has_value());
  auto* ur_provision_factor_ptr =
      std::get_if<Real>(&reserve_provision.ur_provision_factor.value());
  REQUIRE(ur_provision_factor_ptr != nullptr);
  CHECK(*ur_provision_factor_ptr == 1.0);

  REQUIRE(reserve_provision.dr_provision_factor.has_value());
  auto* dr_provision_factor_ptr =
      std::get_if<Real>(&reserve_provision.dr_provision_factor.value());
  REQUIRE(dr_provision_factor_ptr != nullptr);
  CHECK(*dr_provision_factor_ptr == 1.0);

  // Check costs
  REQUIRE(reserve_provision.urcost.has_value());
  auto* urcost_ptr = std::get_if<Real>(&reserve_provision.urcost.value());
  REQUIRE(urcost_ptr != nullptr);
  CHECK(*urcost_ptr == 10.0);

  REQUIRE(reserve_provision.drcost.has_value());
  auto* drcost_ptr = std::get_if<Real>(&reserve_provision.drcost.value());
  REQUIRE(drcost_ptr != nullptr);
  CHECK(*drcost_ptr == 8.0);
}

TEST_CASE("ReserveProvision with time-varying factors")
{
  ReserveProvision reserve_provision;

  // Create vectors for time-varying provision factors
  std::vector<Real> ur_provision_factor_schedule = {0.9, 1.0, 1.0, 0.9};
  std::vector<Real> dr_provision_factor_schedule = {0.8, 0.9, 1.0, 0.8};

  // Assign to reserve provision
  reserve_provision.ur_provision_factor = ur_provision_factor_schedule;
  reserve_provision.dr_provision_factor = dr_provision_factor_schedule;

  // Verify schedules were assigned properly
  REQUIRE(reserve_provision.ur_provision_factor.has_value());
  REQUIRE(reserve_provision.dr_provision_factor.has_value());

  // Check that we have vectors, not scalars
  auto* ur_provision_factor_vec_ptr = std::get_if<std::vector<Real>>(
      &reserve_provision.ur_provision_factor.value());
  auto* dr_provision_factor_vec_ptr = std::get_if<std::vector<Real>>(
      &reserve_provision.dr_provision_factor.value());

  REQUIRE(ur_provision_factor_vec_ptr != nullptr);
  REQUIRE(dr_provision_factor_vec_ptr != nullptr);

  // Check the schedule values
  CHECK(ur_provision_factor_vec_ptr->size() == 4);
  CHECK(dr_provision_factor_vec_ptr->size() == 4);

  CHECK((*ur_provision_factor_vec_ptr)[0] == 0.9);
  CHECK((*ur_provision_factor_vec_ptr)[1] == 1.0);
  CHECK((*ur_provision_factor_vec_ptr)[2] == 1.0);
  CHECK((*ur_provision_factor_vec_ptr)[3] == 0.9);

  CHECK((*dr_provision_factor_vec_ptr)[0] == 0.8);
  CHECK((*dr_provision_factor_vec_ptr)[1] == 0.9);
  CHECK((*dr_provision_factor_vec_ptr)[2] == 1.0);
  CHECK((*dr_provision_factor_vec_ptr)[3] == 0.8);
}
