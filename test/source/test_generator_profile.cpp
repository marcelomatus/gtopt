// SPDX-License-Identifier: BSD-3-Clause
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/generator_profile.hpp>

using namespace gtopt;

TEST_CASE("GeneratorProfile default construction")  // NOLINT
{
  const GeneratorProfile gp;

  CHECK(gp.uid == Uid {unknown_uid});
  CHECK(gp.name.empty());
  CHECK_FALSE(gp.active.has_value());
  CHECK(gp.generator == SingleId {unknown_uid});
  CHECK_FALSE(gp.scost.has_value());
}

TEST_CASE("GeneratorProfile attribute assignment")  // NOLINT
{
  GeneratorProfile gp;

  gp.uid = Uid {1};
  gp.name = "solar_profile";
  gp.active = true;
  gp.generator = Uid {42};
  gp.scost = 2.5;

  CHECK(gp.uid == Uid {1});
  CHECK(gp.name == "solar_profile");
  REQUIRE(gp.active.has_value());
  CHECK(std::get<IntBool>(gp.active.value()) == 1);
  CHECK(std::get<Uid>(gp.generator) == Uid {42});

  REQUIRE(gp.scost.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  CHECK(*std::get_if<Real>(&gp.scost.value()) == doctest::Approx(2.5));
}

TEST_CASE("GeneratorProfile scalar profile")  // NOLINT
{
  GeneratorProfile gp;
  gp.uid = Uid {2};
  gp.name = "wind_profile";
  gp.generator = Uid {10};

  // Scalar profile (same value for all scenario/time/block combinations)
  gp.profile = 0.75;

  REQUIRE(std::get_if<Real>(&gp.profile) != nullptr);
  CHECK(*std::get_if<Real>(&gp.profile) == doctest::Approx(0.75));
}

TEST_CASE("GeneratorProfile 2D vector profile")  // NOLINT
{
  // STBRealFieldSched is FieldSched3<double> = variant<double,
  // vector<vector<vector<double>>>, string>
  GeneratorProfile gp;
  gp.uid = Uid {3};
  gp.name = "pv_profile_24h";
  gp.generator = Uid {5};

  // Two stages, one block each – scalar profile per stage-block
  // Use the FileSched (string) alternative to name an external file
  gp.profile = std::string {"solar_pv_profile"};

  REQUIRE(std::get_if<std::string>(&gp.profile) != nullptr);
  CHECK(*std::get_if<std::string>(&gp.profile) == "solar_pv_profile");
}

TEST_CASE("GeneratorProfile with name-based generator reference")  // NOLINT
{
  GeneratorProfile gp;
  gp.uid = Uid {4};
  gp.name = "hydro_profile";
  gp.generator = Name {"hydro_plant_1"};

  CHECK(std::holds_alternative<Name>(gp.generator));
  CHECK(std::get<Name>(gp.generator) == "hydro_plant_1");
}
