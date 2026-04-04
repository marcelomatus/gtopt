// SPDX-License-Identifier: BSD-3-Clause
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/junction.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway.hpp>

TEST_CASE("Junction construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Junction junction;

  CHECK(junction.uid == Uid {unknown_uid});
  CHECK(junction.name == Name {});
  CHECK_FALSE(junction.active.has_value());
  CHECK_FALSE(junction.drain.has_value());
}

TEST_CASE("Junction attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Junction junction;

  junction.uid = 7001;
  junction.name = "TestJunction";
  junction.active = true;
  junction.drain = true;

  CHECK(junction.uid == 7001);
  CHECK(junction.name == "TestJunction");
  CHECK(std::get<IntBool>(junction.active.value()) == 1);
  REQUIRE(junction.drain.has_value());
  CHECK(junction.drain.value() == true);
}

TEST_CASE("Junction with drain disabled")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Junction junction;

  junction.uid = 7002;
  junction.name = "NonDrainJunction";
  junction.drain = false;

  CHECK(junction.uid == 7002);
  REQUIRE(junction.drain.has_value());
  CHECK(junction.drain.value() == false);
}

TEST_CASE("Turbine construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Turbine turbine;

  CHECK(turbine.uid == Uid {unknown_uid});
  CHECK(turbine.name == Name {});
  CHECK_FALSE(turbine.active.has_value());

  CHECK_FALSE(turbine.waterway.has_value());
  CHECK(turbine.generator == SingleId {unknown_uid});

  CHECK_FALSE(turbine.drain.has_value());
  CHECK_FALSE(turbine.conversion_rate.has_value());
  CHECK_FALSE(turbine.capacity.has_value());
}

TEST_CASE("Turbine attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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

  CHECK(std::get<Uid>(turbine.waterway.value()) == Uid {1001});
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
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

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

TEST_CASE("Waterway construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Waterway waterway;

  CHECK(waterway.uid == Uid {unknown_uid});
  CHECK(waterway.name == Name {});
  CHECK_FALSE(waterway.active.has_value());

  CHECK(waterway.junction_a == SingleId {unknown_uid});
  CHECK(waterway.junction_b == SingleId {unknown_uid});

  CHECK_FALSE(waterway.capacity.has_value());

  // lossfactor has a default of 0.0
  REQUIRE(waterway.lossfactor.has_value());
  CHECK(std::get<Real>(waterway.lossfactor.value_or(Real {-1.0})) == 0.0);

  // fmin has a default of 0.0
  REQUIRE(waterway.fmin.has_value());
  CHECK(std::get<Real>(waterway.fmin.value_or(Real {-1.0})) == 0.0);

  // fmax has a default of 300000.0
  REQUIRE(waterway.fmax.has_value());
  CHECK(std::get<Real>(waterway.fmax.value_or(Real {0.0})) == 300'000.0);
}

TEST_CASE("Waterway attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Waterway waterway;

  waterway.uid = 6001;
  waterway.name = "TestWaterway";
  waterway.active = true;

  waterway.junction_a = Uid {101};
  waterway.junction_b = Uid {102};

  waterway.capacity = 1000.0;
  waterway.lossfactor = 0.05;
  waterway.fmin = 10.0;
  waterway.fmax = 500.0;

  CHECK(waterway.uid == 6001);
  CHECK(waterway.name == "TestWaterway");
  CHECK(std::get<IntBool>(waterway.active.value()) == 1);

  CHECK(std::get<Uid>(waterway.junction_a) == Uid {101});
  CHECK(std::get<Uid>(waterway.junction_b) == Uid {102});

  CHECK(*std::get_if<Real>(&waterway.capacity.value()) == 1000.0);
  CHECK(*std::get_if<Real>(&waterway.lossfactor.value()) == 0.05);
  CHECK(*std::get_if<Real>(&waterway.fmin.value()) == 10.0);
  CHECK(*std::get_if<Real>(&waterway.fmax.value()) == 500.0);
}

TEST_CASE("Waterway with time-block flow limits")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Waterway waterway;

  // OptTBRealFieldSched uses 2D vectors (time x block)
  std::vector<std::vector<Real>> fmin_schedule = {{5.0, 10.0}, {15.0, 8.0}};
  std::vector<std::vector<Real>> fmax_schedule = {
      {400.0, 500.0},
      {600.0, 450.0},
  };

  waterway.fmin = fmin_schedule;
  waterway.fmax = fmax_schedule;

  REQUIRE(waterway.fmin.has_value());
  REQUIRE(waterway.fmax.has_value());

  auto* fmin_vec_ptr =
      std::get_if<std::vector<std::vector<Real>>>(&waterway.fmin.value());
  auto* fmax_vec_ptr =
      std::get_if<std::vector<std::vector<Real>>>(&waterway.fmax.value());

  REQUIRE(fmin_vec_ptr != nullptr);
  REQUIRE(fmax_vec_ptr != nullptr);

  CHECK(fmin_vec_ptr->size() == 2);
  CHECK(fmax_vec_ptr->size() == 2);

  CHECK((*fmin_vec_ptr)[0][0] == 5.0);
  CHECK((*fmin_vec_ptr)[1][1] == 8.0);
  CHECK((*fmax_vec_ptr)[0][0] == 400.0);
  CHECK((*fmax_vec_ptr)[1][1] == 450.0);
}

TEST_CASE("Reservoir construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Reservoir reservoir;

  CHECK(reservoir.uid == Uid {unknown_uid});
  CHECK(reservoir.name == Name {});
  CHECK_FALSE(reservoir.active.has_value());

  CHECK(reservoir.junction == SingleId {unknown_uid});

  // Check defaults with values
  REQUIRE(reservoir.spillway_capacity.has_value());
  CHECK(reservoir.spillway_capacity.value_or(0.0) == doctest::Approx(6000.0));

  CHECK_FALSE(reservoir.spillway_cost.has_value());

  CHECK_FALSE(reservoir.capacity.has_value());
  CHECK_FALSE(reservoir.annual_loss.has_value());
  CHECK_FALSE(reservoir.emin.has_value());
  CHECK_FALSE(reservoir.emax.has_value());
  CHECK_FALSE(reservoir.ecost.has_value());
  CHECK_FALSE(reservoir.eini.has_value());
  CHECK_FALSE(reservoir.efin.has_value());

  REQUIRE(reservoir.fmin.has_value());
  CHECK(reservoir.fmin.value_or(0.0)
        == doctest::Approx(Reservoir::default_fmin));

  REQUIRE(reservoir.fmax.has_value());
  CHECK(reservoir.fmax.value_or(0.0)
        == doctest::Approx(Reservoir::default_fmax));

  CHECK_FALSE(reservoir.energy_scale.has_value());

  REQUIRE(reservoir.flow_conversion_rate.has_value());
  CHECK(reservoir.flow_conversion_rate.value_or(0.0)
        == doctest::Approx(Reservoir::default_flow_conversion_rate));

  // use_state_variable defaults to nullopt (coupled by value_or(true))
  CHECK_FALSE(reservoir.use_state_variable.has_value());
  CHECK(reservoir.use_state_variable.value_or(true) == true);
}

TEST_CASE("Reservoir attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Reservoir reservoir;

  reservoir.uid = 9001;
  reservoir.name = "TestReservoir";
  reservoir.active = true;

  reservoir.junction = Uid {7001};

  reservoir.spillway_capacity = 8000.0;
  reservoir.spillway_cost = 5.0;

  reservoir.capacity = 50000.0;
  reservoir.annual_loss = 0.02;
  reservoir.emin = 10000.0;
  reservoir.emax = 45000.0;
  reservoir.ecost = 1.5;
  reservoir.eini = 25000.0;
  reservoir.efin = 20000.0;

  reservoir.energy_scale = 2.0;
  reservoir.flow_conversion_rate = 0.0036;

  CHECK(reservoir.uid == 9001);
  CHECK(reservoir.name == "TestReservoir");
  CHECK(std::get<IntBool>(reservoir.active.value()) == 1);

  CHECK(std::get<Uid>(reservoir.junction) == Uid {7001});

  CHECK(reservoir.spillway_capacity.value_or(0.0) == doctest::Approx(8000.0));
  CHECK(reservoir.spillway_cost.value_or(0.0) == doctest::Approx(5.0));

  CHECK(*std::get_if<Real>(&reservoir.capacity.value())
        == doctest::Approx(50000.0));
  CHECK(*std::get_if<Real>(&reservoir.annual_loss.value())
        == doctest::Approx(0.02));
  CHECK(*std::get_if<Real>(&reservoir.emin.value())
        == doctest::Approx(10000.0));
  CHECK(*std::get_if<Real>(&reservoir.emax.value())
        == doctest::Approx(45000.0));
  CHECK(*std::get_if<Real>(&reservoir.ecost.value()) == doctest::Approx(1.5));
  CHECK(reservoir.eini.value_or(0.0) == doctest::Approx(25000.0));
  CHECK(reservoir.efin.value_or(0.0) == doctest::Approx(20000.0));

  CHECK(reservoir.energy_scale.value_or(0.0) == doctest::Approx(2.0));
  CHECK(reservoir.flow_conversion_rate.value_or(0.0)
        == doctest::Approx(0.0036));
}

TEST_CASE("Reservoir with time-varying volume limits")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Reservoir reservoir;

  std::vector<Real> emin_schedule = {8000.0, 10000.0, 12000.0, 9000.0};
  std::vector<Real> emax_schedule = {40000.0, 45000.0, 50000.0, 42000.0};

  reservoir.emin = emin_schedule;
  reservoir.emax = emax_schedule;

  REQUIRE(reservoir.emin.has_value());
  REQUIRE(reservoir.emax.has_value());

  auto* emin_vec_ptr = std::get_if<std::vector<Real>>(&reservoir.emin.value());
  auto* emax_vec_ptr = std::get_if<std::vector<Real>>(&reservoir.emax.value());

  REQUIRE(emin_vec_ptr != nullptr);
  REQUIRE(emax_vec_ptr != nullptr);

  CHECK(emin_vec_ptr->size() == 4);
  CHECK(emax_vec_ptr->size() == 4);

  CHECK((*emin_vec_ptr)[0] == doctest::Approx(8000.0));
  CHECK((*emin_vec_ptr)[3] == doctest::Approx(9000.0));
  CHECK((*emax_vec_ptr)[0] == doctest::Approx(40000.0));
  CHECK((*emax_vec_ptr)[3] == doctest::Approx(42000.0));
}

TEST_CASE("Reservoir use_state_variable defaults and explicit set")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default is nullopt (coupled by convention)")
  {
    const Reservoir rsv;
    CHECK_FALSE(rsv.use_state_variable.has_value());
    // value_or(true) reflects the reservoir-LP default: coupled
    CHECK(rsv.use_state_variable.value_or(true) == true);
  }

  SUBCASE("can be set to false (decoupled)")
  {
    Reservoir rsv;
    rsv.use_state_variable = false;
    REQUIRE(rsv.use_state_variable.has_value());
    CHECK(rsv.use_state_variable.value_or(true) == false);
  }

  SUBCASE("can be set to true (explicitly coupled)")
  {
    Reservoir rsv;
    rsv.use_state_variable = true;
    REQUIRE(rsv.use_state_variable.has_value());
    CHECK(rsv.use_state_variable.value_or(false) == true);
  }

  SUBCASE("daily_cycle default is nullopt")
  {
    const Reservoir rsv;
    CHECK_FALSE(rsv.daily_cycle.has_value());
    // Reservoir LP defaults to daily_cycle=false when not set
    CHECK(rsv.daily_cycle.value_or(false) == false);
  }

  SUBCASE("daily_cycle can be set to true")
  {
    Reservoir rsv;
    rsv.daily_cycle = true;
    REQUIRE(rsv.daily_cycle.has_value());
    CHECK(rsv.daily_cycle.value_or(false) == true);
  }

  SUBCASE("daily_cycle can be set to false")
  {
    Reservoir rsv;
    rsv.daily_cycle = false;
    REQUIRE(rsv.daily_cycle.has_value());
    CHECK(rsv.daily_cycle.value_or(true) == false);
  }
}
