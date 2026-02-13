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
  CHECK_FALSE(reservoir.vmin.has_value());
  CHECK_FALSE(reservoir.vmax.has_value());
  CHECK_FALSE(reservoir.vcost.has_value());
  CHECK_FALSE(reservoir.vini.has_value());
  CHECK_FALSE(reservoir.vfin.has_value());

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
  reservoir.vmin = 10000.0;
  reservoir.vmax = 45000.0;
  reservoir.vcost = 1.5;
  reservoir.vini = 25000.0;
  reservoir.vfin = 20000.0;

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
  CHECK(*std::get_if<Real>(&reservoir.vmin.value())
        == doctest::Approx(10000.0));
  CHECK(*std::get_if<Real>(&reservoir.vmax.value())
        == doctest::Approx(45000.0));
  CHECK(*std::get_if<Real>(&reservoir.vcost.value()) == doctest::Approx(1.5));
  CHECK(reservoir.vini.value_or(0.0) == doctest::Approx(25000.0));
  CHECK(reservoir.vfin.value_or(0.0) == doctest::Approx(20000.0));

  CHECK(reservoir.vol_scale.value_or(0.0) == doctest::Approx(2.0));
  CHECK(reservoir.flow_conversion_rate.value_or(0.0)
        == doctest::Approx(0.0036));
}

TEST_CASE("Reservoir with time-varying volume limits")
{
  Reservoir reservoir;

  std::vector<Real> vmin_schedule = {8000.0, 10000.0, 12000.0, 9000.0};
  std::vector<Real> vmax_schedule = {40000.0, 45000.0, 50000.0, 42000.0};

  reservoir.vmin = vmin_schedule;
  reservoir.vmax = vmax_schedule;

  REQUIRE(reservoir.vmin.has_value());
  REQUIRE(reservoir.vmax.has_value());

  auto* vmin_vec_ptr = std::get_if<std::vector<Real>>(&reservoir.vmin.value());
  auto* vmax_vec_ptr = std::get_if<std::vector<Real>>(&reservoir.vmax.value());

  REQUIRE(vmin_vec_ptr != nullptr);
  REQUIRE(vmax_vec_ptr != nullptr);

  CHECK(vmin_vec_ptr->size() == 4);
  CHECK(vmax_vec_ptr->size() == 4);

  CHECK((*vmin_vec_ptr)[0] == doctest::Approx(8000.0));
  CHECK((*vmin_vec_ptr)[3] == doctest::Approx(9000.0));
  CHECK((*vmax_vec_ptr)[0] == doctest::Approx(40000.0));
  CHECK((*vmax_vec_ptr)[3] == doctest::Approx(42000.0));
}
