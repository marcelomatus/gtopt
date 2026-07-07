// SPDX-License-Identifier: BSD-3-Clause
//
// JSON round-trip tests for ``HydrogenStorage`` + ``HydrogenNode``.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/json/json_hydrogen_node.hpp>
#include <gtopt/json/json_hydrogen_storage.hpp>

using namespace gtopt;

TEST_CASE("HydrogenNode JSON round-trip")  // NOLINT
{
  std::string_view js = R"({"uid":11,"name":"h2_node_1"})";
  const HydrogenNode hn = daw::json::from_json<HydrogenNode>(js);
  CHECK(hn.uid == Uid {11});
  CHECK(hn.name == "h2_node_1");
}

TEST_CASE("HydrogenStorage JSON round-trip — salt cavern")  // NOLINT
{
  // 200 GWh_LHV salt cavern with cushion-gas floor.
  std::string_view js = R"({
    "uid": 1,
    "name": "salt_cavern_atacama",
    "type": "salt_cavern",
    "hydrogen_node": "h2_node_1",
    "input_efficiency": 0.95,
    "output_efficiency": 0.99,
    "annual_loss": 0.005,
    "emin": 50000.0,
    "emax": 200000.0,
    "eini": 100000.0,
    "efin": 80000.0,
    "capacity": 200000.0,
    "use_state_variable": true,
    "daily_cycle": false
  })";
  const HydrogenStorage hs = daw::json::from_json<HydrogenStorage>(js);

  CHECK(hs.uid == Uid {1});
  CHECK(hs.name == "salt_cavern_atacama");
  CHECK(hs.type.value_or(Name {}) == "salt_cavern");
  REQUIRE(hs.hydrogen_node.has_value());
  REQUIRE(std::holds_alternative<Name>(*hs.hydrogen_node));
  CHECK(std::get<Name>(*hs.hydrogen_node) == "h2_node_1");
  CHECK(hs.eini.value_or(-1.0) == doctest::Approx(100000.0));
  CHECK(hs.efin.value_or(-1.0) == doctest::Approx(80000.0));
}
