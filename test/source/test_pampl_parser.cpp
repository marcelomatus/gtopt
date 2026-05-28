/**
 * @file      test_pampl_parser.hpp
 * @brief     Unit tests for PamplParser
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <variant>
#include <vector>

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
    CHECK_FALSE(ucs[0].penalty.has_value());  // no clause ⇒ hard
  }

  // ── Header: penalty clause → soft constraint ─────────────────────────────
  TEST_CASE("constraint header with penalty sets soft cost")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    SUBCASE("penalty after name")
    {
      const auto& ucs = PamplParser::parse(
                            "constraint gas_cap penalty 500: "
                            "generator('G1').generation <= 100;")
                            .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK(ucs[0].name == "gas_cap");
      REQUIRE(ucs[0].penalty.has_value());
      CHECK(ucs[0].penalty.value_or(-1.0) == doctest::Approx(500.0));
    }

    SUBCASE("penalty after description")
    {
      const auto& ucs = PamplParser::parse(
                            "constraint gas_cap \"daily gas\" penalty 71.45: "
                            "generator('G1').generation <= 100;")
                            .constraints;
      REQUIRE(ucs.size() == 1);
      REQUIRE(ucs[0].description.has_value());
      CHECK(ucs[0].penalty.value_or(-1.0) == doctest::Approx(71.45));
    }

    SUBCASE("inactive + penalty")
    {
      const auto& ucs =
          PamplParser::parse(
              "inactive constraint c penalty 10: line('L1').flow <= 5;")
              .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK(ucs[0].active.value_or(true) == false);
      CHECK(ucs[0].penalty.value_or(-1.0) == doctest::Approx(10.0));
    }

    SUBCASE("penalty references a scalar param")
    {
      const auto& ucs = PamplParser::parse(
                            "param fuel_cap_penalty = 500;\n"
                            "constraint gas_cap penalty fuel_cap_penalty: "
                            "generator('G1').generation <= 100;")
                            .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK(ucs[0].penalty.value_or(-1.0) == doctest::Approx(500.0));
    }

    SUBCASE("penalty referencing an unknown param raises")
    {
      CHECK_THROWS((void)PamplParser::parse(
          "constraint c penalty nope: line('L1').flow <= 5;"));
    }

    SUBCASE("missing penalty/colon raises")
    {
      CHECK_THROWS((void)PamplParser::parse(
          "constraint c bogus generator('G1').generation <= 1;"));
    }
  }

  // ── Header: rhs clause → per-block (scheduled) RHS ───────────────────────
  TEST_CASE("constraint header with rhs clause sets per-block schedule")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    SUBCASE("rhs vector maps onto single-row TB matrix [[...]]")
    {
      const auto& ucs = PamplParser::parse(
                            "constraint ramp_cap rhs [40, 40, 60, 60]: "
                            "generator('RALCO').generation <= 0;")
                            .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK(ucs[0].name == "ramp_cap");
      REQUIRE(ucs[0].rhs.has_value());
      REQUIRE(std::holds_alternative<std::vector<std::vector<double>>>(
          ucs[0].rhs.value()));
      const auto& mat =
          std::get<std::vector<std::vector<double>>>(ucs[0].rhs.value());
      REQUIRE(mat.size() == 1);
      REQUIRE(mat[0].size() == 4);
      CHECK(mat[0][0] == doctest::Approx(40.0));
      CHECK(mat[0][2] == doctest::Approx(60.0));
      // The inline scalar tail survives as the per-block fallback.
      CHECK(ucs[0].expression.find("<= 0") != std::string::npos);
    }

    SUBCASE("rhs after penalty, any order")
    {
      const auto& ucs = PamplParser::parse(
                            "constraint c penalty 500 rhs [1.5, 2.5]: "
                            "line('L1').flow <= 0;")
                            .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK(ucs[0].penalty.value_or(-1.0) == doctest::Approx(500.0));
      REQUIRE(ucs[0].rhs.has_value());
      const auto& mat =
          std::get<std::vector<std::vector<double>>>(ucs[0].rhs.value());
      REQUIRE(mat[0].size() == 2);
      CHECK(mat[0][1] == doctest::Approx(2.5));
    }

    SUBCASE("rhs before penalty, any order")
    {
      const auto& ucs =
          PamplParser::parse(
              "constraint c rhs [3, 4] penalty 10: line('L1').flow <= 0;")
              .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK(ucs[0].penalty.value_or(-1.0) == doctest::Approx(10.0));
      REQUIRE(ucs[0].rhs.has_value());
    }

    SUBCASE("rhs values may be param-value expressions")
    {
      const auto& ucs =
          PamplParser::parse(
              "param cap = 30;\n"
              "constraint c rhs [cap, cap * 2]: line('L1').flow <= 0;")
              .constraints;
      REQUIRE(ucs.size() == 1);
      const auto& mat =
          std::get<std::vector<std::vector<double>>>(ucs[0].rhs.value());
      CHECK(mat[0][0] == doctest::Approx(30.0));
      CHECK(mat[0][1] == doctest::Approx(60.0));
    }

    SUBCASE("empty rhs vector raises")
    {
      CHECK_THROWS((void)PamplParser::parse(
          "constraint c rhs []: line('L1').flow <= 0;"));
    }

    SUBCASE("duplicate rhs clause raises")
    {
      CHECK_THROWS((void)PamplParser::parse(
          "constraint c rhs [1] rhs [2]: line('L1').flow <= 0;"));
    }

    SUBCASE("scalar-RHS constraint leaves rhs unset (unchanged)")
    {
      const auto& ucs =
          PamplParser::parse("constraint c: line('L1').flow <= 200;")
              .constraints;
      REQUIRE(ucs.size() == 1);
      CHECK_FALSE(ucs[0].rhs.has_value());
    }
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

  TEST_CASE("Scalar param value supports arithmetic + param refs")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    SUBCASE("division chain (1000/24/7)")
    {
      const auto r = PamplParser::parse("param p = 1000 / 24 / 7;");
      CHECK(r.params[0].value.value_or(0) == doctest::Approx(1000.0 / 24 / 7));
    }
    SUBCASE("precedence: * / before + -")
    {
      const auto r = PamplParser::parse("param p = 2 + 3 * 4 - 1;");
      CHECK(r.params[0].value.value_or(0) == doctest::Approx(13.0));
    }
    SUBCASE("parentheses override precedence")
    {
      const auto r = PamplParser::parse("param p = 1000 / (24 * 7);");
      CHECK(r.params[0].value.value_or(0)
            == doctest::Approx(1000.0 / (24 * 7)));
    }
    SUBCASE("param references an earlier param (a=1800; b=a/7)")
    {
      const auto r = PamplParser::parse("param a = 1800;\nparam b = a / 7;");
      REQUIRE(r.params.size() == 2);
      CHECK(r.params[1].value.value_or(0) == doctest::Approx(1800.0 / 7));
    }
    SUBCASE("unary minus on a parenthesised param ref: -(a/4) + 1/24")
    {
      const auto r =
          PamplParser::parse("param a = 1800;\nparam b = -(a / 4) + 1 / 24;");
      REQUIRE(r.params.size() == 2);
      CHECK(r.params[1].value.value_or(0)
            == doctest::Approx(-(1800.0 / 4) + (1.0 / 24)));
    }

    SUBCASE("forward reference to a later param raises")
    {
      CHECK_THROWS(
          (void)PamplParser::parse("param b = a / 7;\nparam a = 1800;"));
    }
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
    // NOLINTBEGIN(bugprone-unchecked-optional-access)

    const auto result = PamplParser::parse("param offset = -42.5;");
    REQUIRE(result.params.size() == 1);
    CHECK(result.params[0].value.value_or(0) == doctest::Approx(-42.5));
  }
}

// NOLINTEND(bugprone-unchecked-optional-access)