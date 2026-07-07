// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the ``ThermalStorage`` struct + ``ThermalNode``
// type tag.  Validates default values, field assignment, and the
// carrier compile-time tag.  LP-side behaviour (SoC carry across
// blocks, efin slack, etc.) inherits from ``StorageLP<>`` and is
// already covered by the Battery / Reservoir test suites.

#include <doctest/doctest.h>
#include <gtopt/carrier.hpp>
#include <gtopt/thermal_node.hpp>
#include <gtopt/thermal_storage.hpp>

using namespace gtopt;

TEST_CASE("ThermalNode default values + carrier tag")  // NOLINT
{
  const ThermalNode tn;
  CHECK(tn.uid == Uid {unknown_uid});
  CHECK(tn.name == Name {});
  CHECK_FALSE(tn.active.has_value());
  // Compile-time carrier tag.
  static_assert(ThermalNode::carrier == Carrier::Thermal);
  CHECK(ThermalNode::class_name == LPClassName {"ThermalNode"});
}

TEST_CASE("ThermalStorage default values")  // NOLINT
{
  const ThermalStorage ts;

  CHECK(ts.uid == Uid {unknown_uid});
  CHECK(ts.name == Name {});
  CHECK_FALSE(ts.active.has_value());
  CHECK_FALSE(ts.type.has_value());
  CHECK_FALSE(ts.thermal_node.has_value());

  CHECK_FALSE(ts.input_efficiency.has_value());
  CHECK_FALSE(ts.output_efficiency.has_value());
  CHECK_FALSE(ts.annual_loss.has_value());

  CHECK_FALSE(ts.emin.has_value());
  CHECK_FALSE(ts.emax.has_value());
  CHECK_FALSE(ts.ecost.has_value());
  CHECK_FALSE(ts.eini.has_value());
  CHECK_FALSE(ts.efin.has_value());
  CHECK_FALSE(ts.efin_cost.has_value());

  CHECK_FALSE(ts.capacity.has_value());
  CHECK_FALSE(ts.expcap.has_value());
  CHECK_FALSE(ts.expmod.has_value());

  CHECK(ThermalStorage::class_name == LPClassName {"ThermalStorage"});
}

TEST_CASE("ThermalStorage attribute assignment")  // NOLINT
{
  ThermalStorage ts;
  ts.uid = Uid {42};
  ts.name = "tes_1";
  ts.thermal_node = SingleId {Uid {7}};
  ts.eini = 100.0;
  ts.efin = 50.0;
  ts.efin_cost = 250.0;

  CHECK(ts.uid == Uid {42});
  CHECK(ts.name == "tes_1");
  REQUIRE(ts.thermal_node.has_value());
  CHECK(std::holds_alternative<Uid>(*ts.thermal_node));
  CHECK(std::get<Uid>(*ts.thermal_node) == Uid {7});
  CHECK(ts.eini.value_or(-1.0) == doctest::Approx(100.0));
  CHECK(ts.efin.value_or(-1.0) == doctest::Approx(50.0));
  CHECK(ts.efin_cost.value_or(-1.0) == doctest::Approx(250.0));
}

TEST_CASE("Carrier enum ordering + to_string")  // NOLINT
{
  CHECK(static_cast<int>(Carrier::Electric) == 0);
  CHECK(static_cast<int>(Carrier::Water) == 1);
  CHECK(static_cast<int>(Carrier::Hydrogen) == 2);
  CHECK(static_cast<int>(Carrier::Thermal) == 3);

  CHECK(to_string(Carrier::Electric) == "electric");
  CHECK(to_string(Carrier::Water) == "water");
  CHECK(to_string(Carrier::Hydrogen) == "hydrogen");
  CHECK(to_string(Carrier::Thermal) == "thermal");
}
