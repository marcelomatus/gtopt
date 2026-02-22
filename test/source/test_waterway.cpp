#include <doctest/doctest.h>
#include <gtopt/waterway.hpp>

using namespace gtopt;

TEST_CASE("Waterway construction and default values")
{
  const Waterway waterway;

  CHECK(waterway.uid == Uid {unknown_uid});
  CHECK(waterway.name == Name {});
  CHECK_FALSE(waterway.active.has_value());

  CHECK(waterway.junction_a == SingleId {unknown_uid});
  CHECK(waterway.junction_b == SingleId {unknown_uid});

  CHECK_FALSE(waterway.capacity.has_value());

  // lossfactor has a default of 0.0
  REQUIRE(waterway.lossfactor.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  CHECK(*std::get_if<Real>(&waterway.lossfactor.value()) == 0.0);

  // fmin has a default of 0.0
  REQUIRE(waterway.fmin.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  CHECK(*std::get_if<Real>(&waterway.fmin.value()) == 0.0);

  // fmax has a default of 300000.0
  REQUIRE(waterway.fmax.has_value());

  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  CHECK(*std::get_if<Real>(&waterway.fmax.value()) == 300'000.0);
}

TEST_CASE("Waterway attribute assignment")
{
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
