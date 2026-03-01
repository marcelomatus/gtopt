#include <doctest/doctest.h>
#include <gtopt/reservoir.hpp>

using namespace gtopt;

TEST_CASE("Reservoir construction and default values")
{
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
  CHECK_FALSE(reservoir.vcost.has_value());
  CHECK_FALSE(reservoir.eini.has_value());
  CHECK_FALSE(reservoir.efin.has_value());

  REQUIRE(reservoir.fmin.has_value());
  CHECK(reservoir.fmin.value_or(0.0) == doctest::Approx(-10'000.0));

  REQUIRE(reservoir.fmax.has_value());
  CHECK(reservoir.fmax.value_or(0.0) == doctest::Approx(10'000.0));

  REQUIRE(reservoir.vol_scale.has_value());
  CHECK(reservoir.vol_scale.value_or(0.0) == doctest::Approx(1.0));

  REQUIRE(reservoir.flow_conversion_rate.has_value());
  CHECK(reservoir.flow_conversion_rate.value_or(0.0)
        == doctest::Approx(0.0036));
}

TEST_CASE("Reservoir attribute assignment")
{
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
  reservoir.vcost = 1.5;
  reservoir.eini = 25000.0;
  reservoir.efin = 20000.0;

  reservoir.vol_scale = 2.0;
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
  CHECK(*std::get_if<Real>(&reservoir.vcost.value()) == doctest::Approx(1.5));
  CHECK(reservoir.eini.value_or(0.0) == doctest::Approx(25000.0));
  CHECK(reservoir.efin.value_or(0.0) == doctest::Approx(20000.0));

  CHECK(reservoir.vol_scale.value_or(0.0) == doctest::Approx(2.0));
  CHECK(reservoir.flow_conversion_rate.value_or(0.0)
        == doctest::Approx(0.0036));
}

TEST_CASE("Reservoir with time-varying volume limits")
{
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
