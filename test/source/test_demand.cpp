#include <doctest/doctest.h>
#include <gtopt/demand.hpp>
#include <gtopt/json/json_demand.hpp>

TEST_CASE("Demand")
{
  using namespace gtopt;

  Demand demand = {.uid = 1, .name = "demand_1"};

  CHECK(demand.uid == 1);
  CHECK(demand.name == "demand_1");
}

TEST_CASE("Demand")
{
  using namespace gtopt;

  Demand demand = {.uid = 1, .name = "demand_1", .capacity = 100.0};

  CHECK(demand.uid == 1);
  CHECK(demand.name == "demand_1");

  CHECK(std::get<double>(demand.capacity.value()) == 100.0);
}
