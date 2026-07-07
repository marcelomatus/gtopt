// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file   test_constraint_directive.cpp
 * @brief  Unit tests for ConstraintDirective scaffold (P0 Step 1).
 *
 * Covers the no-behavior-change scaffold for typed constraint-family
 * directives: enum NamedEnum round-trip, ``valid_for_kind()`` invariant,
 * ``effective_penalty`` override semantics, ``implies_daily_sum()``
 * decision table, and JSON round-trip on a ``UserConstraint`` that
 * carries the directive sibling field.
 *
 * LP-side behaviour (effective_penalty, daily_sum override) is exercised
 * here through the helper methods; full LP-build integration tests land
 * with the converter migration steps (RegRange → Site 1, Gas_MaxOpDay →
 * Site 2, MinProvision → Site 3).
 */

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/constraint_directive.hpp>
#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;

namespace test_constraint_directive
{

TEST_CASE("DirectiveKind NamedEnum round-trip")  // NOLINT
{
  SUBCASE("every entry parses back to itself")
  {
    for (const auto& entry : directive_kind_entries) {
      const auto round = enum_from_name<DirectiveKind>(entry.name);
      REQUIRE(round.has_value());
      CHECK(*round == entry.value);
      CHECK(enum_name(entry.value) == entry.name);
    }
  }

  SUBCASE("ASCII case-insensitive lookup")
  {
    CHECK(enum_from_name<DirectiveKind>("REGRANGE")
              .value_or(DirectiveKind::HydroFloor)
          == DirectiveKind::RegRange);
    CHECK(enum_from_name<DirectiveKind>("Daily_Budget")
              .value_or(DirectiveKind::HydroFloor)
          == DirectiveKind::DailyBudget);
  }

  SUBCASE("unknown name returns nullopt")
  {
    CHECK_FALSE(enum_from_name<DirectiveKind>("bogus").has_value());
    CHECK_FALSE(enum_from_name<DirectiveKind>("").has_value());
  }
}

TEST_CASE(
    "ConstraintDirective::valid_for_kind enforces kind invariant")  // NOLINT
{
  SUBCASE("RegRange — penalty allowed, others forbidden")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::RegRange,
        .penalty = 1000.0,
    }
              .valid_for_kind());

    CHECK(ConstraintDirective {
        .kind = DirectiveKind::RegRange,
    }
              .valid_for_kind());

    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::RegRange,
        .scope = Name {"fuel:GAS"},
    }
                    .valid_for_kind());

    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::RegRange,
        .window_hours = Int {24},
    }
                    .valid_for_kind());
  }

  SUBCASE("ReserveProvSum — same shape as RegRange")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::ReserveProvSum,
        .penalty = 1000.0,
    }
              .valid_for_kind());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::ReserveProvSum,
        .window_hours = Int {168},
    }
                    .valid_for_kind());
  }

  SUBCASE("DailyBudget — scope optional, penalty optional, window forbidden")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::DailyBudget,
        .scope = Name {"fuel:GAS"},
    }
              .valid_for_kind());
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::DailyBudget,
        .penalty = 10.0,
        .scope = Name {"owner:CSF"},
    }
              .valid_for_kind());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::DailyBudget,
        .window_hours = Int {24},
    }
                    .valid_for_kind());
  }

  SUBCASE("HydroFloor — penalty forbidden, scope carries gate ref")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::HydroFloor,
        .scope = Name {"commitment:1337"},
    }
              .valid_for_kind());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::HydroFloor,
        .penalty = 10.0,
    }
                    .valid_for_kind());
  }

  SUBCASE("MaxStartsWindow — window_hours required and positive")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {24},
    }
              .valid_for_kind());
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {168},
    }
              .valid_for_kind());

    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
    }
                    .valid_for_kind());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {0},
    }
                    .valid_for_kind());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {-1},
    }
                    .valid_for_kind());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .penalty = 5.0,
        .window_hours = Int {24},
    }
                    .valid_for_kind());
  }
}

TEST_CASE(
    "ConstraintDirective::effective_penalty override semantics")  // NOLINT
{
  const ConstraintDirective d_with {
      .kind = DirectiveKind::RegRange,
      .penalty = 1000.0,
  };
  const ConstraintDirective d_without {
      .kind = DirectiveKind::RegRange,
  };

  SUBCASE("directive penalty wins when set")
  {
    CHECK(d_with.effective_penalty(std::nullopt).value_or(-1.0)
          == doctest::Approx(1000.0));
    CHECK(d_with.effective_penalty(10.0).value_or(-1.0)
          == doctest::Approx(1000.0));
  }

  SUBCASE("falls back to scalar when directive omits penalty")
  {
    CHECK_FALSE(d_without.effective_penalty(std::nullopt).has_value());
    CHECK(d_without.effective_penalty(42.0).value_or(-1.0)
          == doctest::Approx(42.0));
  }
}

TEST_CASE("ConstraintDirective::implies_daily_sum decision table")  // NOLINT
{
  SUBCASE("DailyBudget always implies daily_sum")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::DailyBudget,
    }
              .implies_daily_sum());
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::DailyBudget,
        .scope = Name {"fuel:GAS"},
    }
              .implies_daily_sum());
  }

  SUBCASE("MaxStartsWindow only when window_hours == 24")
  {
    CHECK(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {24},
    }
              .implies_daily_sum());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {168},
    }
                    .implies_daily_sum());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::MaxStartsWindow,
        .window_hours = Int {1},
    }
                    .implies_daily_sum());
  }

  SUBCASE("other kinds never imply daily_sum")
  {
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::RegRange,
    }
                    .implies_daily_sum());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::ReserveProvSum,
    }
                    .implies_daily_sum());
    CHECK_FALSE(ConstraintDirective {
        .kind = DirectiveKind::HydroFloor,
    }
                    .implies_daily_sum());
  }
}

TEST_CASE("UserConstraint JSON round-trip — directive sibling field")  // NOLINT
{
  SUBCASE("absent directive → field omitted on write, std::nullopt on read")
  {
    UserConstraint uc {
        .uid = Uid {1},
        .name = "uc1",
        .expression = "generator(\"G1\").generation <= 100",
    };
    const auto json = daw::json::to_json(uc);

    // No "directive" key in the JSON when none was set.
    CHECK(json.find("directive") == std::string::npos);

    const auto round = daw::json::from_json<UserConstraint>(json);
    CHECK_FALSE(round.directive.has_value());
  }

  SUBCASE("RegRange directive round-trips with penalty override")
  {
    UserConstraint uc {
        .uid = Uid {2},
        .name = "regrange_uc",
        .expression = "commitment(\"uc_G1\").status <= 0",
        .directive =
            ConstraintDirective {
                .kind = DirectiveKind::RegRange,
                .penalty = 1000.0,
            },
    };
    const auto json = daw::json::to_json(uc);
    CHECK(json.find("\"directive\"") != std::string::npos);
    CHECK(json.find("\"regrange\"") != std::string::npos);
    CHECK(json.find("\"penalty\":1000") != std::string::npos);

    const auto round = daw::json::from_json<UserConstraint>(json);
    REQUIRE(round.directive.has_value());
    CHECK(round.directive->kind == DirectiveKind::RegRange);
    CHECK(round.directive->penalty.value_or(-1.0) == doctest::Approx(1000.0));
    CHECK_FALSE(round.directive->scope.has_value());
    CHECK_FALSE(round.directive->window_hours.has_value());
  }

  SUBCASE("DailyBudget directive round-trips with scope")
  {
    UserConstraint uc {
        .uid = Uid {3},
        .name = "gas_maxop",
        .expression = "sum(generator(all: fuel=\"GAS\").generation) <= 800",
        .directive =
            ConstraintDirective {
                .kind = DirectiveKind::DailyBudget,
                .scope = Name {"fuel:GAS"},
            },
    };
    const auto json = daw::json::to_json(uc);
    const auto round = daw::json::from_json<UserConstraint>(json);
    REQUIRE(round.directive.has_value());
    CHECK(round.directive->kind == DirectiveKind::DailyBudget);
    CHECK(round.directive->scope.value_or("") == "fuel:GAS");
    CHECK_FALSE(round.directive->penalty.has_value());
    CHECK_FALSE(round.directive->window_hours.has_value());
    CHECK(round.directive->implies_daily_sum());
  }

  SUBCASE("MaxStartsWindow directive round-trips with window_hours")
  {
    UserConstraint uc {
        .uid = Uid {4},
        .name = "max_starts_24h",
        .expression = "sum(commitment(\"uc_G1\").startup) <= 5",
        .directive =
            ConstraintDirective {
                .kind = DirectiveKind::MaxStartsWindow,
                .window_hours = Int {24},
            },
    };
    const auto json = daw::json::to_json(uc);
    const auto round = daw::json::from_json<UserConstraint>(json);
    REQUIRE(round.directive.has_value());
    CHECK(round.directive->kind == DirectiveKind::MaxStartsWindow);
    CHECK(round.directive->window_hours.value_or(-1) == 24);
    CHECK(round.directive->implies_daily_sum());
  }
}

}  // namespace test_constraint_directive
