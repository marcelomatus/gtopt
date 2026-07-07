// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the ``CarrierConverter`` data struct + Carrier enum
// round-trip.

#include <doctest/doctest.h>
#include <gtopt/carrier.hpp>
#include <gtopt/carrier_converter.hpp>
#include <gtopt/enum_option.hpp>

using namespace gtopt;

TEST_CASE("CarrierConverter default values")  // NOLINT
{
  const CarrierConverter cc;
  CHECK(cc.uid == Uid {unknown_uid});
  CHECK(cc.name == Name {});
  CHECK_FALSE(cc.active.has_value());
  CHECK_FALSE(cc.type.has_value());
  CHECK(cc.from_carrier == Carrier::Electric);
  CHECK(cc.to_carrier == Carrier::Electric);
  CHECK_FALSE(cc.from_node.has_value());
  CHECK_FALSE(cc.to_node.has_value());
  CHECK_FALSE(cc.efficiency.has_value());
  CHECK_FALSE(cc.ocost.has_value());
  CHECK_FALSE(cc.capacity.has_value());
  CHECK(CarrierConverter::class_name == LPClassName {"CarrierConverter"});
}

TEST_CASE("CarrierConverter attribute assignment — electrolyser")  // NOLINT
{
  CarrierConverter cc;
  cc.uid = Uid {1};
  cc.name = "pem_electrolyser";
  cc.type = "electrolyser";
  cc.from_carrier = Carrier::Electric;
  cc.to_carrier = Carrier::Hydrogen;
  cc.from_node = SingleId {Uid {1}};
  cc.to_node = SingleId {Uid {11}};

  CHECK(cc.uid == Uid {1});
  CHECK(cc.from_carrier == Carrier::Electric);
  CHECK(cc.to_carrier == Carrier::Hydrogen);
  REQUIRE(cc.from_node.has_value());
  CHECK(std::get<Uid>(*cc.from_node) == Uid {1});
  REQUIRE(cc.to_node.has_value());
  CHECK(std::get<Uid>(*cc.to_node) == Uid {11});
}

TEST_CASE("CarrierConverter attribute assignment — Haber-Bosch")  // NOLINT
{
  CarrierConverter cc;
  cc.from_carrier = Carrier::Hydrogen;
  cc.to_carrier = Carrier::Ammonia;
  cc.type = "haber_bosch";
  CHECK(cc.from_carrier == Carrier::Hydrogen);
  CHECK(cc.to_carrier == Carrier::Ammonia);
}

TEST_CASE(
    "Carrier enum round-trips through enum_name / require_enum")  // NOLINT
{
  CHECK(enum_name(Carrier::Electric) == "electric");
  CHECK(enum_name(Carrier::Water) == "water");
  CHECK(enum_name(Carrier::Hydrogen) == "hydrogen");
  CHECK(enum_name(Carrier::Thermal) == "thermal");
  CHECK(enum_name(Carrier::Ammonia) == "ammonia");

  CHECK(require_enum<Carrier>("c", "electric") == Carrier::Electric);
  CHECK(require_enum<Carrier>("c", "hydrogen") == Carrier::Hydrogen);
  CHECK(require_enum<Carrier>("c", "ammonia") == Carrier::Ammonia);
  CHECK(require_enum<Carrier>("c", "thermal") == Carrier::Thermal);

  // Unknown strings throw with an "(expected: …)" hint.
  CHECK_THROWS_AS(
      [[maybe_unused]] auto v = require_enum<Carrier>("c", "nonsense"),
      std::invalid_argument);
}
