// SPDX-License-Identifier: BSD-3-Clause
#include <unordered_map>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/user_param.hpp>

TEST_CASE("UserParam construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const UserParam param;

  CHECK(param.name == Name {});
  CHECK_FALSE(param.value.has_value());
  CHECK_FALSE(param.monthly.has_value());
}

TEST_CASE("UserParam with scalar value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  UserParam param;
  param.name = "pct_elec";
  param.value = 35.0;

  CHECK(param.name == "pct_elec");
  REQUIRE(param.value.has_value());
  CHECK(param.value.value_or(0.0) == doctest::Approx(35.0));
  CHECK_FALSE(param.monthly.has_value());
}

TEST_CASE("UserParam with monthly values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  UserParam param;
  param.name = "irr_seasonal";
  param.monthly =
      std::vector<Real> {0, 0, 0, 100, 100, 100, 100, 100, 100, 100, 0, 0};

  CHECK(param.name == "irr_seasonal");
  CHECK_FALSE(param.value.has_value());
  REQUIRE(param.monthly.has_value());
  CHECK(param.monthly->size() == 12);

  // Check specific monthly values
  CHECK((*param.monthly)[0] == doctest::Approx(0.0));
  CHECK((*param.monthly)[3] == doctest::Approx(100.0));
  CHECK((*param.monthly)[9] == doctest::Approx(100.0));
  CHECK((*param.monthly)[11] == doctest::Approx(0.0));
}

TEST_CASE("UserParam designated initializer construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const UserParam param {
      .name = "max_gen",
      .value = 500.0,
  };

  CHECK(param.name == "max_gen");
  CHECK(param.value.value_or(0.0) == doctest::Approx(500.0));
}

TEST_CASE("UserParamMap lookup")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  UserParamMap params;

  params["pct_elec"] = UserParam {
      .name = "pct_elec",
      .value = 35.0,
  };
  params["irr_seasonal"] = UserParam {
      .name = "irr_seasonal",
      .monthly =
          std::vector<Real> {0, 0, 0, 100, 100, 100, 100, 100, 100, 100, 0, 0},
  };

  CHECK(params.size() == 2);
  CHECK(params.contains("pct_elec"));
  CHECK(params.contains("irr_seasonal"));
  CHECK_FALSE(params.contains("nonexistent"));

  CHECK(params["pct_elec"].value.value_or(0.0) == doctest::Approx(35.0));
  REQUIRE(params["irr_seasonal"].monthly.has_value());
  CHECK(params["irr_seasonal"].monthly->size() == 12);
}

TEST_CASE("UserParam with zero value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const UserParam param {
      .name = "zero_param",
      .value = 0.0,
  };

  REQUIRE(param.value.has_value());
  CHECK(param.value.value_or(1.0) == doctest::Approx(0.0));
}

TEST_CASE("UserParam with negative value")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const UserParam param {
      .name = "neg_param",
      .value = -42.5,
  };

  REQUIRE(param.value.has_value());
  CHECK(param.value.value_or(0.0) == doctest::Approx(-42.5));
}
