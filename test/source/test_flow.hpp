// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/flow.hpp>

TEST_CASE("Flow construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Flow flow;

  CHECK(flow.uid == Uid {unknown_uid});
  CHECK(flow.name == Name {});
  CHECK_FALSE(flow.active.has_value());

  // Default direction is +1 (inflow)
  REQUIRE(flow.direction.has_value());
  CHECK(flow.direction.value_or(0) == 1);

  CHECK_FALSE(flow.junction.has_value());
}

TEST_CASE("Flow is_input method")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default direction is inflow")
  {
    const Flow flow;
    CHECK(flow.is_input() == true);
  }

  SUBCASE("positive direction is inflow")
  {
    Flow flow;
    flow.direction = 1;
    CHECK(flow.is_input() == true);
  }

  SUBCASE("negative direction is outflow")
  {
    Flow flow;
    flow.direction = -1;
    CHECK(flow.is_input() == false);
  }

  SUBCASE("zero direction treated as inflow")
  {
    Flow flow;
    flow.direction = 0;
    CHECK(flow.is_input() == true);
  }

  SUBCASE("unset direction defaults to inflow")
  {
    Flow flow;
    flow.direction = std::nullopt;
    CHECK(flow.is_input() == true);
  }
}

TEST_CASE("Flow attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Flow flow;

  flow.uid = 1001;
  flow.name = "river_inflow";
  flow.active = true;
  flow.direction = 1;
  flow.junction = Uid {7001};
  flow.discharge = 150.0;

  CHECK(flow.uid == 1001);
  CHECK(flow.name == "river_inflow");
  CHECK(std::get<IntBool>(flow.active.value()) == 1);
  CHECK(flow.direction.value_or(0) == 1);
  CHECK(std::get<Uid>(flow.junction.value()) == Uid {7001});
}

TEST_CASE("Flow designated initializer construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Flow flow {
      .uid = Uid {2},
      .name = "env_discharge",
      .active = {},
      .direction = -1,
      .junction = SingleId {Uid {5}},
      .discharge = 50.0,
  };

  CHECK(flow.uid == Uid {2});
  CHECK(flow.name == "env_discharge");
  CHECK(flow.direction.value_or(0) == -1);
  CHECK(flow.is_input() == false);
  CHECK(std::get<Uid>(flow.junction.value()) == Uid {5});
}

TEST_CASE("Flow with vector discharge schedule")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Flow flow;
  flow.uid = 3;
  flow.name = "seasonal_inflow";

  // STBRealFieldSched = scenario x stage x block discharge
  std::vector<std::vector<std::vector<Real>>> schedule {
      {{100.0, 120.0}, {80.0, 90.0}},
      {{110.0, 130.0}, {85.0, 95.0}},
  };
  flow.discharge = schedule;

  auto* vec_ptr =
      std::get_if<std::vector<std::vector<std::vector<Real>>>>(&flow.discharge);
  REQUIRE(vec_ptr != nullptr);
  CHECK(vec_ptr->size() == 2);
  CHECK((*vec_ptr)[0][0][0] == doctest::Approx(100.0));
  CHECK((*vec_ptr)[1][1][1] == doctest::Approx(95.0));
}

TEST_CASE("Flow with file schedule")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Flow flow;
  flow.uid = 4;
  flow.name = "file_inflow";
  flow.discharge = std::string {"inflow_data"};

  auto* file_ptr = std::get_if<std::string>(&flow.discharge);
  REQUIRE(file_ptr != nullptr);
  CHECK(*file_ptr == "inflow_data");
}

TEST_CASE("Flow without junction (flow-turbine mode)")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Flow flow {
      .uid = Uid {5},
      .name = "pasada_flow",
      .active = {},
      .direction = {},
      .junction = {},
      .discharge = 200.0,
  };

  CHECK_FALSE(flow.junction.has_value());
  CHECK(flow.uid == Uid {5});
}
