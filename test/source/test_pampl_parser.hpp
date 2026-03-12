/**
 * @file      test_pampl_parser.hpp
 * @brief     Unit tests for PamplParser
 * @date      Thu Mar 12 00:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#pragma once

#include <doctest/doctest.h>
#include <gtopt/pampl_parser.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_SUITE("PamplParser")
{
  // ── Bare expression (no header) ───────────────────────────────────────────

  TEST_CASE("Bare expression assigns uid and auto-name")
  {
    const auto ucs =
        PamplParser::parse("generator('G1').generation <= 100;", Uid {1});

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
    const auto ucs = PamplParser::parse(
        "constraint gen_limit: generator('G1').generation <= 100;");

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
    CHECK(ucs[0].active.value_or(true) == true);
    CHECK_FALSE(ucs[0].description.has_value());
  }

  // ── Header: constraint NAME "desc" : ─────────────────────────────────────

  TEST_CASE("constraint header with description (double quotes)")
  {
    const auto ucs = PamplParser::parse(
        "constraint gen_limit \"Max generation for G1\": "
        "generator('G1').generation <= 100;");

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
    REQUIRE(ucs[0].description.has_value());
    CHECK(ucs[0].description.value_or("") == "Max generation for G1");
  }

  TEST_CASE("constraint header with description (single quotes)")
  {
    const auto ucs = PamplParser::parse(
        "constraint gen_limit 'Max generation for G1': "
        "generator('G1').generation <= 100;");

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
    REQUIRE(ucs[0].description.has_value());
    CHECK(ucs[0].description.value_or("") == "Max generation for G1");
  }

  // ── inactive keyword ─────────────────────────────────────────────────────

  TEST_CASE("inactive constraint sets active=false")
  {
    const auto ucs = PamplParser::parse(
        "inactive constraint flow_check: line('L1').flow <= 200;");

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "flow_check");
    CHECK(ucs[0].active.value_or(true) == false);
  }

  // ── Multiple constraints ──────────────────────────────────────────────────

  TEST_CASE("Multiple constraints are parsed in order")
  {
    const std::string src = R"(
constraint first: generator('G1').generation <= 100;
inactive constraint second: demand('D1').load >= 50;
sum(generator(all).generation) <= 1000;
)";
    const auto ucs = PamplParser::parse(src, Uid {10});

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
    const std::string src = R"(
# This is a comment
constraint gen_limit:
  # another comment
  generator('G1').generation <= 100; # inline comment
)";
    const auto ucs = PamplParser::parse(src);

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
  }

  TEST_CASE("Double-slash comments are ignored")
  {
    const std::string src = R"(
// Double-slash comment
constraint gen_limit:
  generator('G1').generation <= 100;
)";
    const auto ucs = PamplParser::parse(src);

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].name == "gen_limit");
  }

  // ── UIDs ─────────────────────────────────────────────────────────────────

  TEST_CASE("UID sequence starts at start_uid")
  {
    const auto ucs = PamplParser::parse(
        "constraint a: generator('G1').generation <= 100;\n"
        "constraint b: demand('D1').load >= 50;",
        Uid {5});

    REQUIRE(ucs.size() == 2);
    CHECK(ucs[0].uid == 5);
    CHECK(ucs[1].uid == 6);
  }

  // ── Expression content preservation ─────────────────────────────────────

  TEST_CASE("For-clause in expression is preserved")
  {
    const auto ucs = PamplParser::parse(
        "constraint domain_limit:\n"
        "  generator('G1').generation <= 100, for(stage in {1,2,3});");

    REQUIRE(ucs.size() == 1);
    CHECK(ucs[0].expression.find("for") != std::string::npos);
    CHECK(ucs[0].expression.find("{1,2,3}") != std::string::npos);
  }

  // ── Single quote in element ID ────────────────────────────────────────────

  TEST_CASE("Single-quote element id in expression")
  {
    const auto ucs =
        PamplParser::parse("constraint q: generator('G1').generation <= 100;");
    REQUIRE(ucs.size() == 1);
    // Expression is passed verbatim to ConstraintParser — single-quote
    // support is tested in test_constraint_parser.hpp
    CHECK(ucs[0].expression.find("G1") != std::string::npos);
  }

  // ── Empty file ───────────────────────────────────────────────────────────

  TEST_CASE("Empty source produces empty result")
  {
    CHECK(PamplParser::parse("").empty());
    CHECK(PamplParser::parse("  # just a comment\n  ").empty());
  }

  // ── Error cases ──────────────────────────────────────────────────────────

  TEST_CASE("Missing semicolon throws")
  {
    CHECK_THROWS_AS(PamplParser::parse("generator('G1').generation <= 100"),
                    std::invalid_argument);
  }

  TEST_CASE("Missing constraint name after 'constraint' keyword throws")
  {
    CHECK_THROWS_AS(
        PamplParser::parse("constraint : generator('G1').generation <= 100;"),
        std::invalid_argument);
  }

  TEST_CASE("'inactive' without 'constraint' keyword throws")
  {
    CHECK_THROWS_AS(
        PamplParser::parse("inactive oops: generator('G1').generation <= 100;"),
        std::invalid_argument);
  }
}
