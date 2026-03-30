/**
 * @file      test_scale_alpha.hpp
 * @brief     Unit tests for the scale_alpha SDDP option
 * @date      2026-03-30
 * @copyright BSD-3-Clause
 *
 * Tests:
 *  1. Default-constructed SddpOptions has no scale_alpha set
 *  2. PlanningOptionsLP applies the compiled default (1'000'000)
 *  3. Explicit scale_alpha value overrides the default
 *  4. Merge behaviour for scale_alpha
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

TEST_CASE("scale_alpha - default-constructed SddpOptions has nullopt")
{
  const SddpOptions opts {};
  CHECK_FALSE(opts.scale_alpha.has_value());
}

TEST_CASE("scale_alpha - PlanningOptionsLP applies compiled default")
{
  const PlanningOptions planning_opts {};
  const PlanningOptionsLP lp_opts {
      planning_opts,
  };

  CHECK(lp_opts.sddp_scale_alpha()
        == doctest::Approx(PlanningOptionsLP::default_sddp_scale_alpha));
  CHECK(lp_opts.sddp_scale_alpha() == doctest::Approx(1'000'000.0));
}

TEST_CASE("scale_alpha - explicit value overrides default")
{
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
  SddpOptions base {
      .scale_alpha = 1000.0,
  };
  SddpOptions overlay {};

  base.merge(std::move(overlay));

  REQUIRE(base.scale_alpha.has_value());
  CHECK(base.scale_alpha.value_or(0.0) == doctest::Approx(1000.0));
}

}  // namespace
