// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the ``HydrogenStorage`` + ``HydrogenNode`` types.
// Mirrors ``test_thermal_storage.cpp``.

#include <doctest/doctest.h>
#include <gtopt/carrier.hpp>
#include <gtopt/hydrogen_node.hpp>
#include <gtopt/hydrogen_storage.hpp>

using namespace gtopt;

TEST_CASE("HydrogenNode default values + carrier tag")  // NOLINT
{
  const HydrogenNode hn;
  CHECK(hn.uid == Uid {unknown_uid});
  CHECK(hn.name == Name {});
  CHECK_FALSE(hn.active.has_value());
  static_assert(HydrogenNode::carrier == Carrier::Hydrogen);
  CHECK(HydrogenNode::class_name == LPClassName {"HydrogenNode"});
}

TEST_CASE("HydrogenStorage default values")  // NOLINT
{
  const HydrogenStorage hs;
  CHECK(hs.uid == Uid {unknown_uid});
  CHECK(hs.name == Name {});
  CHECK_FALSE(hs.active.has_value());
  CHECK_FALSE(hs.type.has_value());
  CHECK_FALSE(hs.hydrogen_node.has_value());
  CHECK_FALSE(hs.input_efficiency.has_value());
  CHECK_FALSE(hs.output_efficiency.has_value());
  CHECK_FALSE(hs.annual_loss.has_value());
  CHECK_FALSE(hs.emin.has_value());
  CHECK_FALSE(hs.emax.has_value());
  CHECK_FALSE(hs.eini.has_value());
  CHECK_FALSE(hs.efin.has_value());
  CHECK_FALSE(hs.capacity.has_value());
  CHECK(HydrogenStorage::class_name == LPClassName {"HydrogenStorage"});
}

TEST_CASE("HydrogenStorage attribute assignment")  // NOLINT
{
  HydrogenStorage hs;
  hs.uid = Uid {99};
  hs.name = "salt_cavern_1";
  hs.type = "salt_cavern";
  hs.hydrogen_node = SingleId {Uid {11}};
  hs.eini = 50000.0;
  hs.efin = 20000.0;
  hs.efin_cost = 100.0;

  CHECK(hs.uid == Uid {99});
  CHECK(hs.name == "salt_cavern_1");
  CHECK(hs.type.value_or(Name {}) == "salt_cavern");
  REQUIRE(hs.hydrogen_node.has_value());
  CHECK(std::holds_alternative<Uid>(*hs.hydrogen_node));
  CHECK(std::get<Uid>(*hs.hydrogen_node) == Uid {11});
  CHECK(hs.eini.value_or(-1.0) == doctest::Approx(50000.0));
  CHECK(hs.efin.value_or(-1.0) == doctest::Approx(20000.0));
}

TEST_CASE("Carrier enum includes Hydrogen and Ammonia")  // NOLINT
{
  CHECK(static_cast<int>(Carrier::Hydrogen) == 2);
  CHECK(static_cast<int>(Carrier::Ammonia) == 4);
  CHECK(to_string(Carrier::Hydrogen) == "hydrogen");
  CHECK(to_string(Carrier::Ammonia) == "ammonia");
}
