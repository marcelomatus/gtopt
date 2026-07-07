// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_user_constraint_strict_errors.cpp
 * @brief     Hardened AMPL / UserConstraint resolution: every genuinely
 *            *undefined* reference (unknown element name, unknown
 *            attribute, undefined named parameter, bad bare-uid index,
 *            malformed expression, bad coefficient profile) must produce
 *            an informative error and terminate the build — NOT
 *            warn-and-skip or silently contribute zero.
 *
 *            Regression complement: a *defined-but-offline* element
 *            (pmax==0 on a block ⇒ no LP column there) must still
 *            contribute 0 silently and NOT throw.  That leniency is the
 *            one legitimate exception and is pinned here too.
 *
 * @date      Tue May 27 00:00:00 2026
 * @author    claude
 * @copyright BSD-3-Clause
 */

#include <format>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace uc_strict_errors_test  // unique outer namespace (unity-build safe)
{

// Build a PlanningLP from JSON and return the exception message, or an
// empty string if no exception was thrown.  Construction triggers the LP
// build (UserConstraintLP ctor parses the expression; add_to_lp lowers
// the rows), so both parse errors and resolution errors surface here.
[[nodiscard]] std::string build_error_message(std::string_view json)
{
  try {
    auto planning = parse_planning_json(json);
    PlanningLP planning_lp(std::move(planning));
    auto result = planning_lp.resolve();
    (void)result;
  } catch (const std::exception& ex) {
    return ex.what();
  }
  return {};
}

// clang-format off

/// Minimal single-bus, single-block case.  The `EXPR` placeholder is
/// substituted into the user constraint at the call site to keep the
/// fixtures terse.
[[nodiscard]] std::string make_json(std::string_view uc_name,
                                    std::string_view expr)
{
  static constexpr std::string_view tmpl = R"json(
  {{
    "options": {{
      "annual_discount_rate": 0.0,
      "output_format": "csv",
      "output_compression": "uncompressed",
      "model_options": {{
        "use_single_bus": true,
        "scale_objective": 1,
        "demand_fail_cost": 1000
      }}
    }},
    "simulation": {{
      "block_array": [ {{ "uid": 1, "duration": 1 }} ],
      "stage_array": [ {{ "uid": 1, "first_block": 0, "count_block": 1, "active": 1 }} ],
      "scenario_array": [ {{ "uid": 1, "probability_factor": 1 }} ]
    }},
    "system": {{
      "name": "uc_strict_errors",
      "bus_array": [ {{ "uid": 1, "name": "b1" }} ],
      "generator_array": [
        {{ "uid": 1, "name": "g1", "bus": 1, "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200 }}
      ],
      "demand_array": [
        {{ "uid": 1, "name": "d1", "bus": 1, "lmax": [ [ 80.0 ] ] }}
      ],
      "user_constraint_array": [
        {{ "uid": 1, "name": "{}", "expression": "{}" }}
      ]
    }}
  }})json";
  return std::format(tmpl, uc_name, expr);
}

// clang-format on

TEST_CASE(  // NOLINT
    "UC strict — unknown element NAME throws with UC name + bad name + hint")
{
  const auto msg =
      build_error_message(make_json("uc_bad_name",
                                    "generator('does_not_exist').generation "
                                    "<= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_bad_name") != std::string::npos);
  CHECK(msg.find("does_not_exist") != std::string::npos);
  CHECK(msg.find("unknown generator name") != std::string::npos);
  // Closest registered generator name is offered as a hint.
  CHECK(msg.find("g1") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — known element but UNKNOWN ATTRIBUTE throws with attr list")
{
  const auto msg = build_error_message(
      make_json("uc_bad_attr", "generator('g1').bogus_attr <= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_bad_attr") != std::string::npos);
  CHECK(msg.find("bogus_attr") != std::string::npos);
  CHECK(msg.find("unknown attribute") != std::string::npos);
  // The valid-attribute hint should list the real LP variable
  // (generation) registered on g1.
  CHECK(msg.find("generation") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — undefined named PARAMETER throws with UC name + bad param")
{
  const auto msg = build_error_message(make_json(
      "uc_bad_param", "generator('g1').generation <= nonexistent_param + 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_bad_param") != std::string::npos);
  CHECK(msg.find("nonexistent_param") != std::string::npos);
  CHECK(msg.find("unknown parameter") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — out-of-range bare-uid INDEX throws (no silent zero)")
{
  // generator(999) — uid 999 is not registered.  Must throw, not
  // silently contribute 0.
  const auto msg = build_error_message(
      make_json("uc_bad_index", "generator(999).generation <= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_bad_index") != std::string::npos);
  CHECK(msg.find("999") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — UNKNOWN SINGLETON SCALAR (options.bogus) throws")
{
  const auto msg = build_error_message(
      make_json("uc_bad_scalar",
                "generator('g1').generation <= options.bogus_option + 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_bad_scalar") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — MALFORMED EXPRESSION (parse error) throws with UC name")
{
  // Missing comparison operator.
  const auto msg = build_error_message(
      make_json("uc_parse_err", "generator('g1').generation + 5"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_parse_err") != std::string::npos);
  CHECK(msg.find("parse error") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — non-linear product (two variables) parse error throws")
{
  const auto msg = build_error_message(
      make_json("uc_nonlinear",
                "generator('g1').generation * generator('g1').generation "
                "<= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_nonlinear") != std::string::npos);
  CHECK(msg.find("parse error") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — BAD COEFFICIENT PROFILE (bare profile) parse error throws")
{
  // A coefficient profile must multiply a variable term, not stand alone.
  const auto msg =
      build_error_message(make_json("uc_bad_profile", "[1, 2, 3] <= 0"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_bad_profile") != std::string::npos);
  CHECK(msg.find("parse error") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — empty coefficient profile parse error throws")
{
  const auto msg = build_error_message(
      make_json("uc_empty_profile", "[] * generator('g1').generation <= 0"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_empty_profile") != std::string::npos);
  CHECK(msg.find("parse error") != std::string::npos);
}

TEST_CASE(  // NOLINT
    "UC strict — stray character in expression parse error throws")
{
  const auto msg = build_error_message(
      make_json("uc_stray", "generator('g1').generation @ 100 <= 100"));
  REQUIRE_FALSE(msg.empty());
  CHECK(msg.find("uc_stray") != std::string::npos);
  CHECK(msg.find("parse error") != std::string::npos);
}

// ── REGRESSION: defined-but-offline element must stay a silent 0 ─────────

namespace
{

/// Two-block fixture where g1 has pmax = [0, 100]: GeneratorLP omits the
/// LP column for block 1 (pmax==0) but emits one for block 2.  A UC
/// referencing g1.generation must NOT throw on block 1 — the element is
/// *defined* and *known*, merely offline on that block, so its term
/// legitimately contributes 0.  This is the one leniency the hardening
/// must preserve.
struct OfflineGeneratorFixture
{
  PlanningOptions opts;
  Simulation simulation;
  System system;

  explicit OfflineGeneratorFixture(Array<UserConstraint> ucs)
  {
    opts.model_options.use_single_bus = true;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.model_options.scale_objective = 1.0;

    simulation = Simulation {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1,
                },
                {
                    .uid = Uid {2},
                    .duration = 1,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 2,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {1},
                    .probability_factor = 1.0,
                },
            },
    };

    system = System {
        .name = "uc_offline_generator",
        .bus_array =
            {
                {
                    .uid = Uid {1},
                    .name = "b1",
                },
            },
        .demand_array =
            {
                {
                    .uid = Uid {1},
                    .name = "d1",
                    .bus = Uid {1},
                    .lmax =
                        std::vector<std::vector<double>> {
                            {
                                0.0,
                                50.0,
                            },
                        },
                },
            },
        .generator_array =
            {
                {
                    .uid = Uid {1},
                    .name = "g1",
                    .bus = Uid {1},
                    .pmax =
                        std::vector<std::vector<double>> {
                            {
                                0.0,
                                100.0,
                            },
                        },
                    .gcost = 10.0,
                },
            },
        .user_constraint_array = std::move(ucs),
    };
  }
};

}  // namespace

TEST_CASE(  // NOLINT
    "UC regression — defined-but-offline (pmax=0) element contributes 0, "
    "no throw")
{
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_offline_ok",
          .expression = "generator('g1').generation <= 100",
      },
  };
  const OfflineGeneratorFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  // Block 1 has no g1 column (pmax=0) — the resolver must treat the term
  // as a silent 0 there and build the LP without throwing.  Block 2
  // resolves normally.
  REQUIRE_NOTHROW(SystemLP(fix.system,
                           sim_lp,
                           LpMatrixOptions {
                               .col_with_names = true,
                               .row_with_names = true,
                           }));
}

}  // namespace uc_strict_errors_test
