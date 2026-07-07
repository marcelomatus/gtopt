// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the ``AmmoniaStorage`` + ``AmmoniaNode`` types.
// Mirrors ``test_hydrogen_storage.cpp``.

#include <doctest/doctest.h>
#include <gtopt/ammonia_node.hpp>
#include <gtopt/ammonia_storage.hpp>
#include <gtopt/carrier.hpp>

using namespace gtopt;

TEST_CASE("AmmoniaNode default values + carrier tag")  // NOLINT
{
  const AmmoniaNode an;
  CHECK(an.uid == Uid {unknown_uid});
  CHECK(an.name == Name {});
  CHECK_FALSE(an.active.has_value());
  static_assert(AmmoniaNode::carrier == Carrier::Ammonia);
  CHECK(AmmoniaNode::class_name == LPClassName {"AmmoniaNode"});
}

TEST_CASE("AmmoniaStorage default values")  // NOLINT
{
  const AmmoniaStorage as;
  CHECK(as.uid == Uid {unknown_uid});
  CHECK(as.name == Name {});
  CHECK_FALSE(as.active.has_value());
  CHECK_FALSE(as.type.has_value());
  CHECK_FALSE(as.ammonia_node.has_value());
  CHECK_FALSE(as.input_efficiency.has_value());
  CHECK_FALSE(as.output_efficiency.has_value());
  CHECK_FALSE(as.annual_loss.has_value());
  CHECK_FALSE(as.emin.has_value());
  CHECK_FALSE(as.emax.has_value());
  CHECK_FALSE(as.eini.has_value());
  CHECK_FALSE(as.efin.has_value());
  CHECK_FALSE(as.capacity.has_value());
  CHECK(AmmoniaStorage::class_name == LPClassName {"AmmoniaStorage"});
}

TEST_CASE("AmmoniaStorage attribute assignment — refrigerated tank")  // NOLINT
{
  AmmoniaStorage as;
  as.uid = Uid {77};
  as.name = "atacama_nh3_tank";
  as.type = "refrigerated";
  as.ammonia_node = SingleId {Uid {22}};
  // 60 kt-NH3 ≈ 310 GWh_LHV (5.17 kWh/kg × 60 000 t × 1000 kg/t / 1e6)
  as.eini = 155000.0;  // half-full (in MWh_LHV).
  as.emax = 310000.0;
  as.annual_loss = 0.025;  // ~0.07% per day boil-off.

  CHECK(as.uid == Uid {77});
  CHECK(as.name == "atacama_nh3_tank");
  CHECK(as.type.value_or(Name {}) == "refrigerated");
  REQUIRE(as.ammonia_node.has_value());
  CHECK(std::holds_alternative<Uid>(*as.ammonia_node));
  CHECK(std::get<Uid>(*as.ammonia_node) == Uid {22});
  CHECK(as.eini.value_or(-1.0) == doctest::Approx(155000.0));
}
