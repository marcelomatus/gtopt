/**
 * @file      test_convergence_mode.hpp
 * @brief     Unit tests for ConvergenceMode enum and related SDDP
 *            convergence options
 * @date      2026-03-30
 * @copyright BSD-3-Clause
 */

#pragma once

#include <array>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/enum_option.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_options.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// --- ConvergenceMode enum_from_name / enum_name ---

TEST_CASE("ConvergenceMode enum_from_name")  // NOLINT
{
  SUBCASE("gap_only parses correctly")
  {
    const auto result = enum_from_name<ConvergenceMode>("gap_only");
    CHECK((result && *result == ConvergenceMode::gap_only));
  }

  SUBCASE("gap_stationary parses correctly")
  {
    const auto result = enum_from_name<ConvergenceMode>("gap_stationary");
    CHECK((result && *result == ConvergenceMode::gap_stationary));
  }

  SUBCASE("statistical parses correctly")
  {
    const auto result = enum_from_name<ConvergenceMode>("statistical");
    CHECK((result && *result == ConvergenceMode::statistical));
  }

  SUBCASE("unknown name returns nullopt")
  {
    const auto result = enum_from_name<ConvergenceMode>("nonexistent");
    CHECK_FALSE(result.has_value());
  }

  SUBCASE("empty name returns nullopt")
  {
    const auto result = enum_from_name<ConvergenceMode>("");
    CHECK_FALSE(result.has_value());
  }
}

TEST_CASE("ConvergenceMode enum_name")  // NOLINT
{
  CHECK(enum_name(ConvergenceMode::gap_only) == "gap_only");
  CHECK(enum_name(ConvergenceMode::gap_stationary) == "gap_stationary");
  CHECK(enum_name(ConvergenceMode::statistical) == "statistical");
}

TEST_CASE("ConvergenceMode entries table has 3 entries")  // NOLINT
{
  const auto entries = enum_entries(ConvergenceMode {});
  CHECK(entries.size() == 3);
}

TEST_CASE(  // NOLINT
    "ConvergenceMode round-trip from_name/enum_name")
{
  constexpr std::array names = {
      std::string_view {"gap_only"},
      std::string_view {"gap_stationary"},
      std::string_view {"statistical"},
  };

  for (const auto name : names) {
    const auto parsed = enum_from_name<ConvergenceMode>(name);
    REQUIRE(parsed.has_value());
    CHECK(enum_name(*parsed) == name);
  }
}

// --- Default values for convergence options ---

TEST_CASE(  // NOLINT
    "SddpOptions convergence defaults are nullopt")
{
  const SddpOptions opts {};

  CHECK_FALSE(opts.convergence_mode.has_value());
  CHECK_FALSE(opts.convergence_tol.has_value());
  CHECK_FALSE(opts.stationary_tol.has_value());
  CHECK_FALSE(opts.stationary_window.has_value());
  CHECK_FALSE(opts.convergence_confidence.has_value());
}

// --- ConvergenceMode underlying integer values ---

TEST_CASE(  // NOLINT
    "ConvergenceMode underlying integer values")
{
  CHECK(static_cast<uint8_t>(ConvergenceMode::gap_only) == 0);
  CHECK(static_cast<uint8_t>(ConvergenceMode::gap_stationary) == 1);
  CHECK(static_cast<uint8_t>(ConvergenceMode::statistical) == 2);
}

// --- Merge behavior for convergence fields ---

TEST_CASE(  // NOLINT
    "SddpOptions merge convergence fields")
{
  SddpOptions base {
      .convergence_tol = 1e-4,
      .convergence_mode = ConvergenceMode::gap_only,
  };

  SddpOptions overlay {
      .convergence_mode = ConvergenceMode::statistical,
      .stationary_tol = 0.01,
      .convergence_confidence = 0.95,
  };

  base.merge(std::move(overlay));

  REQUIRE(base.convergence_mode.has_value());
  CHECK(*base.convergence_mode == ConvergenceMode::statistical);
  CHECK(base.convergence_tol.value_or(0.0) == doctest::Approx(1e-4));
  CHECK(base.stationary_tol.value_or(0.0) == doctest::Approx(0.01));
  CHECK(base.convergence_confidence.value_or(0.0) == doctest::Approx(0.95));
}

TEST_CASE(  // NOLINT
    "SddpOptions merge does not overwrite convergence with nullopt")
{
  SddpOptions base {
      .convergence_mode = ConvergenceMode::gap_stationary,
      .stationary_tol = 0.05,
      .stationary_window = 20,
  };

  SddpOptions empty {};
  base.merge(std::move(empty));

  REQUIRE(base.convergence_mode.has_value());
  CHECK(*base.convergence_mode == ConvergenceMode::gap_stationary);
  CHECK(base.stationary_tol.value_or(0.0) == doctest::Approx(0.05));
  CHECK(base.stationary_window.value_or(0) == 20);
}

// --- Construction with convergence fields ---

TEST_CASE(  // NOLINT
    "SddpOptions construction with all convergence fields")
{
  const SddpOptions opts {
      .convergence_tol = 1e-4,
      .convergence_mode = ConvergenceMode::statistical,
      .stationary_tol = 0.01,
      .stationary_window = 10,
      .convergence_confidence = 0.95,
  };

  REQUIRE(opts.convergence_mode.has_value());
  CHECK(*opts.convergence_mode == ConvergenceMode::statistical);
  CHECK(opts.convergence_tol.value_or(0.0) == doctest::Approx(1e-4));
  CHECK(opts.stationary_tol.value_or(0.0) == doctest::Approx(0.01));
  CHECK(opts.stationary_window.value_or(0) == 10);
  CHECK(opts.convergence_confidence.value_or(0.0) == doctest::Approx(0.95));
}

}  // namespace
