/**
 * @file      test_scale_alpha.hpp
 * @brief     Unit tests for the scale_alpha SDDP option
 * @date      2026-03-30
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Default-constructed SddpOptions has no scale_alpha set
 *  2. PlanningOptionsLP applies the compiled default (10'000'000)
 *  3. Explicit scale_alpha value overrides the default
 *  4. Merge behaviour for scale_alpha
 */

#include <doctest/doctest.h>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_options.hpp>

using namespace gtopt;

namespace
{

using namespace gtopt;

TEST_CASE("scale_alpha - default-constructed SddpOptions has nullopt")
{
  using namespace gtopt;

  const SddpOptions opts {};
  CHECK_FALSE(opts.scale_alpha.has_value());
}

TEST_CASE("scale_alpha - PlanningOptionsLP returns 0 for auto-scale")
{
  using namespace gtopt;

  const PlanningOptions planning_opts {};
  const PlanningOptionsLP lp_opts {
      planning_opts,
  };

  // When not set by user, sddp_scale_alpha() returns 0.0 meaning
  // auto-scale (computed at runtime as max state-variable var_scale).
  CHECK(lp_opts.sddp_scale_alpha() == doctest::Approx(0.0));
}

TEST_CASE("scale_alpha - explicit value overrides default")
{
  using namespace gtopt;

  PlanningOptions planning_opts {
      .sddp_options =
          SddpOptions {
              .scale_alpha = 500.0,
          },
  };
  const PlanningOptionsLP lp_opts {
      planning_opts,
  };

  CHECK(lp_opts.sddp_scale_alpha() == doctest::Approx(500.0));
}

TEST_CASE("scale_alpha - merge fills missing scale_alpha")
{
  using namespace gtopt;

  SddpOptions base {};
  SddpOptions overlay {
      .scale_alpha = 750.0,
  };

  base.merge(std::move(overlay));

  REQUIRE(base.scale_alpha.has_value());
  CHECK(base.scale_alpha.value_or(0.0) == doctest::Approx(750.0));
}

TEST_CASE("scale_alpha - merge overwrites existing scale_alpha")
{
  using namespace gtopt;

  SddpOptions base {
      .scale_alpha = 1000.0,
  };
  SddpOptions overlay {
      .scale_alpha = 500.0,
  };

  base.merge(std::move(overlay));

  REQUIRE(base.scale_alpha.has_value());
  CHECK(base.scale_alpha.value_or(0.0) == doctest::Approx(500.0));
}

TEST_CASE("scale_alpha - merge does not overwrite when overlay is nullopt")
{
  using namespace gtopt;

  SddpOptions base {
      .scale_alpha = 1000.0,
  };
  SddpOptions overlay {};

  base.merge(std::move(overlay));

  REQUIRE(base.scale_alpha.has_value());
  CHECK(base.scale_alpha.value_or(0.0) == doctest::Approx(1000.0));
}

}  // namespace
