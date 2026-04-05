/**
 * @file      test_pampl_parser.hpp
 * @brief     Unit tests for PamplParser
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/pampl_parser.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_SUITE("PamplParser")
{
  // ── Bare expression (no header) ───────────────────────────────────────────

  TEST_CASE("Bare expression assigns uid and auto-name")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto result =
        PamplParser::parse("generator('G1').generation <= 100;", Uid {1});
    const auto& ucs = result.constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].uid == 1);
    CHECK(ucs[0].name == "uc_1");
    CHECK(ucs[0].active.value_or(true) == true);
    CHECK(ucs[0].expression.find("generator") != std::string::npos);
    CHECK(ucs[0].expression.find("<= 100") != std::string::npos);
    CHECK_FALSE(ucs[0].description.has_value());
  }

  // ── Header: constraint NAME : ─────────────────────────────────────────────

  TEST_CASE("constraint header sets name")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs =
        PamplParser::parse(
            "constraint gen_limit: generator('G1').generation <= 100;")
            .constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
    CHECK(ucs[0].active.value_or(true) == true);
    CHECK_FALSE(ucs[0].description.has_value());
  }

  // ── Header: constraint NAME "desc" : ─────────────────────────────────────

  TEST_CASE("constraint header with description (double quotes)")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs = PamplParser::parse(
                          "constraint gen_limit \"Max generation for G1\": "
                          "generator('G1').generation <= 100;")
                          .constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
    REQUIRE(ucs[0].description.has_value());
    CHECK(ucs[0].description.value_or("") == "Max generation for G1");
  }

  TEST_CASE("constraint header with description (single quotes)")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs = PamplParser::parse(
                          "constraint gen_limit 'Max generation for G1': "
                          "generator('G1').generation <= 100;")
                          .constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
    REQUIRE(ucs[0].description.has_value());
    CHECK(ucs[0].description.value_or("") == "Max generation for G1");
  }

  // ── inactive keyword ─────────────────────────────────────────────────────

  TEST_CASE("inactive constraint sets active=false")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs =
        PamplParser::parse(
            "inactive constraint flow_check: line('L1').flow <= 200;")
            .constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "flow_check");
    CHECK(ucs[0].active.value_or(true) == false);
  }

  // ── Multiple constraints ──────────────────────────────────────────────────

  TEST_CASE("Multiple constraints are parsed in order")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const std::string src = R"(
constraint first: generator('G1').generation <= 100;
inactive constraint second: demand('D1').load >= 50;
sum(generator(all).generation) <= 1000;
)";
    const auto& ucs = PamplParser::parse(src, Uid {10}).constraints;

    REQUIRE(ucs.size() == 3);
    CHECK(ucs[0].uid == 10);
    CHECK(ucs[0].name == "first");
    CHECK(ucs[0].active.value_or(true) == true);

    CHECK(ucs[1].uid == 11);
    CHECK(ucs[1].name == "second");
    CHECK(ucs[1].active.value_or(true) == false);

    CHECK(ucs[2].uid == 12);
    CHECK(ucs[2].name == "uc_12");  // auto-generated
  }

  // ── Comments are stripped ────────────────────────────────────────────────

  TEST_CASE("Hash comments are ignored")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const std::string src = R"(
# This is a comment
constraint gen_limit:
  # another comment
  generator('G1').generation <= 100; # inline comment
)";
    const auto& ucs = PamplParser::parse(src).constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
  }

  TEST_CASE("Double-slash comments are ignored")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const std::string src = R"(
// Double-slash comment
constraint gen_limit:
  generator('G1').generation <= 100;
)";
    const auto& ucs = PamplParser::parse(src).constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
  }

  // ── UIDs ─────────────────────────────────────────────────────────────────

  TEST_CASE("UID sequence starts at start_uid")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs = PamplParser::parse(
                          "constraint a: generator('G1').generation <= 100;\n"
                          "constraint b: demand('D1').load >= 50;",
                          Uid {5})
                          .constraints;

    REQUIRE(ucs.size() == 2);
    CHECK(ucs[0].uid == 5);
    CHECK(ucs[1].uid == 6);
  }

  // ── Expression content preservation ─────────────────────────────────────

  TEST_CASE("For-clause in expression is preserved")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs =
        PamplParser::parse(
            "constraint domain_limit:\n"
            "  generator('G1').generation <= 100, for(stage in {1,2,3});")
            .constraints;

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].expression.find("for") != std::string::npos);
    CHECK(ucs[0].expression.find("{1,2,3}") != std::string::npos);
  }

  // ── Single quote in element ID ────────────────────────────────────────────

  TEST_CASE("Single-quote element id in expression")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto& ucs =
        PamplParser::parse("constraint q: generator('G1').generation <= 100;")
            .constraints;
    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].expression.find("G1") != std::string::npos);
  }

  // ── Empty file ───────────────────────────────────────────────────────────

  TEST_CASE("Empty source produces empty result")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK(PamplParser::parse("").constraints.empty());
    CHECK(PamplParser::parse("  # just a comment\n  ").constraints.empty());
  }

  // ── Error cases ──────────────────────────────────────────────────────────

  TEST_CASE("Missing semicolon throws")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        (void)PamplParser::parse("generator('G1').generation <= 100"),
        std::invalid_argument);
  }

  TEST_CASE("Missing constraint name after 'constraint' keyword throws")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS((void)PamplParser::parse(
                        "constraint : generator('G1').generation <= 100;"),
                    std::invalid_argument);
  }

  TEST_CASE("'inactive' without 'constraint' keyword throws")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS((void)PamplParser::parse(
                        "inactive oops: generator('G1').generation <= 100;"),
                    std::invalid_argument);
  }

  // ── Param declarations ──────────────────────────────────────────────────

  TEST_CASE("Scalar param declaration")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto result = PamplParser::parse("param pct_elec = 35;");

    CHECK(result.constraints.empty());
    REQUIRE(result.params.size() == 1);
    CHECK(result.params[0].name == "pct_elec");
    CHECK(result.params[0].value.value_or(0) == doctest::Approx(35.0));
    CHECK_FALSE(result.params[0].monthly.has_value());
  }

  TEST_CASE("Monthly param declaration")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto result = PamplParser::parse(
        "param irr[month] = [0, 0, 0, 100, 100, 100, "
        "100, 100, 100, 100, 0, 0];");

    REQUIRE(result.params.size() == 1);
    CHECK(result.params[0].name == "irr");
    CHECK_FALSE(result.params[0].value.has_value());
    REQUIRE(result.params[0].monthly.has_value());
    CHECK(result.params[0].monthly->size() == 12);
    CHECK((*result.params[0].monthly)[0] == doctest::Approx(0.0));
    CHECK((*result.params[0].monthly)[3] == doctest::Approx(100.0));
    CHECK((*result.params[0].monthly)[11] == doctest::Approx(0.0));
  }

  TEST_CASE("Params and constraints mixed")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const std::string src = R"(
# Define parameters
param limit = 500;
param seasonal[month] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];

# Use parameters in constraints
constraint gen_cap:
  generator('G1').generation <= limit;
)";
    const auto result = PamplParser::parse(src);

    CHECK(result.params.size() == 2);
    CHECK(result.params[0].name == "limit");
    CHECK(result.params[1].name == "seasonal");

    REQUIRE(result.constraints.size() == 1);
    CHECK(result.constraints[0].name == "gen_cap");
    CHECK(result.constraints[0].expression.find("limit") != std::string::npos);
  }

  TEST_CASE("Monthly param with wrong element count throws")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS((void)PamplParser::parse("param bad[month] = [1, 2, 3];"),
                    std::invalid_argument);
  }

  TEST_CASE("Negative param values")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto result = PamplParser::parse("param offset = -42.5;");
    REQUIRE(result.params.size() == 1);
    CHECK(result.params[0].value.value_or(0) == doctest::Approx(-42.5));
  }
}
