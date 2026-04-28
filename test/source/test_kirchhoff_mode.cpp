// SPDX-License-Identifier: BSD-3-Clause
//
// Unit tests for the `KirchhoffMode` enum + `PlanningOptionsLP::
// kirchhoff_mode()` accessor + `LineLP::add_kirchhoff_rows` mode
// dispatch.  Mirrors the `LineLossesMode` test pattern.
//
// PR 1 of the kirchhoff-strategy refactor only registers the enum and
// extracts the existing B–θ assembly into `kirchhoff::node_angle`.
// The `cycle_basis` strategy is enum-reserved but throws on use; PR 2
// will implement it.  These tests pin both behaviours.

#include <doctest/doctest.h>
#include <gtopt/line_enums.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── Enum string round-trip ────────────────────────────────────────

TEST_CASE("KirchhoffMode enum parsing")
{
  SUBCASE("round-trip all values")
  {
    for (const auto& entry : kirchhoff_mode_entries) {
      const auto parsed = enum_from_name<KirchhoffMode>(entry.name);
      REQUIRE(parsed.has_value());
      CHECK(*parsed == entry.value);
      CHECK(enum_name(*parsed) == entry.name);
    }
  }

  SUBCASE("invalid name returns nullopt")
  {
    CHECK_FALSE(enum_from_name<KirchhoffMode>("invalid").has_value());
    CHECK_FALSE(enum_from_name<KirchhoffMode>("").has_value());
  }

  SUBCASE("expected entries present")
  {
    CHECK(kirchhoff_mode_entries.size() == 2);
    CHECK(enum_from_name<KirchhoffMode>("node_angle").value()
          == KirchhoffMode::node_angle);
    CHECK(enum_from_name<KirchhoffMode>("cycle_basis").value()
          == KirchhoffMode::cycle_basis);
  }
}

// ── PlanningOptionsLP::kirchhoff_mode() resolution ────────────────

TEST_CASE("PlanningOptionsLP::kirchhoff_mode default + override")
{
  SUBCASE("unset → node_angle")
  {
    PlanningOptionsLP options {};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::node_angle);
    CHECK(PlanningOptionsLP::default_kirchhoff_mode
          == KirchhoffMode::node_angle);
  }

  SUBCASE("model_options.kirchhoff_mode = 'cycle_basis'")
  {
    PlanningOptions popts;
    popts.model_options.kirchhoff_mode = OptName {"cycle_basis"};
    PlanningOptionsLP options {std::move(popts)};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::cycle_basis);
  }

  SUBCASE("model_options.kirchhoff_mode = 'node_angle' (explicit)")
  {
    PlanningOptions popts;
    popts.model_options.kirchhoff_mode = OptName {"node_angle"};
    PlanningOptionsLP options {std::move(popts)};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::node_angle);
  }

  SUBCASE("invalid string falls back to default")
  {
    PlanningOptions popts;
    popts.model_options.kirchhoff_mode = OptName {"not-a-mode"};
    PlanningOptionsLP options {std::move(popts)};
    CHECK(options.kirchhoff_mode() == KirchhoffMode::node_angle);
  }
}
