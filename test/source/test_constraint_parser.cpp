/**
 * @file      test_constraint_parser.hpp
 * @brief     Unit tests for the ConstraintParser class
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for parsing AMPL-inspired constraint expressions with element
 * references and domain specifications.
 */

#include <doctest/doctest.h>
#include <gtopt/constraint_parser.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_SUITE("ConstraintParser")
{
  // ── Basic constraints ──────────────────────────────────────────────────

  TEST_CASE("Parse simple generator constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(generator("G1").generation <= 100)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "generator");
    CHECK(ref0.element_id == "G1");
    CHECK(ref0.attribute == "generation");
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Parse two-element sum constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("TORO").generation + generator("uid:23").generation <= 300, for(stage in {4,5,6}, block in 1..30))");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(300.0));

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "generator");
    CHECK(ref0.element_id == "TORO");
    CHECK(ref0.attribute == "generation");

    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_type == "generator");
    CHECK(ref1.element_id == "uid:23");
    CHECK(ref1.attribute == "generation");
  }

  TEST_CASE("Parse constraint with coefficients")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(2.5 * generator("G1").generation - 1.5 * demand("D1").load >= 50)");

    CHECK(expr.constraint_type == ConstraintType::GREATER_EQUAL);
    CHECK(expr.rhs == doctest::Approx(50.0));

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "generator");
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.5));
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_type == "demand");
    CHECK(expr.terms[1].coefficient == doctest::Approx(-1.5));
  }

  TEST_CASE("Parse equality constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(generator("G1").generation = 100)");

    CHECK(expr.constraint_type == ConstraintType::EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "generator");
  }

  // ── Domain specifications ──────────────────────────────────────────────

  TEST_CASE("Parse constraint with for clause - explicit set")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(stage in {1,2,3}))");

    CHECK_FALSE(expr.domain.stages.is_all);
    REQUIRE(expr.domain.stages.values.size() == 3);
    CHECK(expr.domain.stages.values[0] == 1);
    CHECK(expr.domain.stages.values[1] == 2);
    CHECK(expr.domain.stages.values[2] == 3);

    // Unspecified dimensions default to all
    CHECK(expr.domain.scenarios.is_all);
    CHECK(expr.domain.blocks.is_all);
  }

  TEST_CASE("Parse constraint with for clause - all keyword")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(stage in all, block in all))");

    CHECK(expr.domain.stages.is_all);
    CHECK(expr.domain.blocks.is_all);
    CHECK(expr.domain.scenarios.is_all);
  }

  TEST_CASE("Parse constraint with scenario domain")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(scenario in {1,2}))");

    CHECK_FALSE(expr.domain.scenarios.is_all);
    REQUIRE(expr.domain.scenarios.values.size() == 2);
    CHECK(expr.domain.scenarios.values[0] == 1);
    CHECK(expr.domain.scenarios.values[1] == 2);
  }

  TEST_CASE("Parse for clause with = in for clause")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(stage = 1, block = 5))");

    REQUIRE(expr.domain.stages.values.size() == 1);
    CHECK(expr.domain.stages.values[0] == 1);
    REQUIRE(expr.domain.blocks.values.size() == 1);
    CHECK(expr.domain.blocks.values[0] == 5);
  }

  TEST_CASE("Parse for clause with all three dimensions")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(scenario in {1,2}, stage in 3..5, block in all))");

    CHECK_FALSE(expr.domain.scenarios.is_all);
    REQUIRE(expr.domain.scenarios.values.size() == 2);
    CHECK_FALSE(expr.domain.stages.is_all);
    REQUIRE(expr.domain.stages.values.size() == 3);
    CHECK(expr.domain.stages.values[0] == 3);
    CHECK(expr.domain.stages.values[2] == 5);
    CHECK(expr.domain.blocks.is_all);
  }

  TEST_CASE("Parse for clause with mixed range and values in braces")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(block in {1, 3..5, 8, 10..12}))");

    CHECK_FALSE(expr.domain.blocks.is_all);
    // 1 + (3,4,5) + 8 + (10,11,12) = 8 values
    REQUIRE(expr.domain.blocks.values.size() == 8);
    CHECK(expr.domain.blocks.values[0] == 1);
    CHECK(expr.domain.blocks.values[1] == 3);
    CHECK(expr.domain.blocks.values[4] == 8);
    CHECK(expr.domain.blocks.values[7] == 12);
  }

  // ── Range constraints ──────────────────────────────────────────────────

  TEST_CASE("Parse range constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(10 <= generator("G1").generation <= 200)");

    CHECK(expr.constraint_type == ConstraintType::RANGE);
    CHECK(expr.lower_bound.value_or(0.0) == doctest::Approx(10.0));
    CHECK(expr.upper_bound.value_or(0.0) == doctest::Approx(200.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
  }

  TEST_CASE("Parse GEQ range constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(200 >= generator("G1").generation >= 10)");

    CHECK(expr.constraint_type == ConstraintType::RANGE);
    CHECK(expr.lower_bound.value_or(0.0) == doctest::Approx(10.0));
    CHECK(expr.upper_bound.value_or(0.0) == doctest::Approx(200.0));
  }

  // ── All element types ──────────────────────────────────────────────────

  TEST_CASE("Parse line flow constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // `line.flow` is a registered *compound* attribute: the AST keeps the
    // reference as-is and the row builder expands it into `flowp - flown`
    // via `SystemContext::add_ampl_compound` at row-assembly time.
    auto expr = ConstraintParser::parse(R"(line("L1_2").flow <= 300)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.element_id == "L1_2");
    CHECK(ref0.attribute == "flow");
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  // ── line.flow compound-attribute preservation ───────────────────────────
  //
  // Lines expose `flow` as a compound attribute registered with
  // `SystemContext::add_ampl_compound("line", "flow", {+flowp, -flown})`.
  // The parser keeps the reference in the AST unchanged; expansion to
  // directional legs happens inside `resolve_col_to_row` when the
  // constraint row is assembled.  These tests pin AST preservation; the
  // row-assembly behavior is covered by the user-constraint integration
  // tests.

  TEST_CASE("Parse line.flow preserves outer coefficient on single term")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(2.5 * line("L1").flow <= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element->attribute == "flow");
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.5));
  }

  TEST_CASE("Parse -line.flow keeps negative sign on single term")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(-line("L1").flow <= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element->attribute == "flow");
    CHECK(expr.terms[0].coefficient == doctest::Approx(-1.0));
  }

  TEST_CASE("Parse line.flow inside range constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(-50 <= line("L1").flow <= 50)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element->element_type == "line");
    CHECK(expr.terms[0].element->attribute == "flow");
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Parse sum(line(all).flow) preserves compound attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(sum(line(all).flow) <= 0)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    CHECK(expr.terms[0].sum_ref->element_type == "line");
    CHECK(expr.terms[0].sum_ref->attribute == "flow");
    CHECK(expr.terms[0].sum_ref->all_elements);
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Parse sum(line(\"L1\",\"L2\").flow) preserves explicit id list")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(sum(line("L1","L2").flow) <= 300)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    CHECK(expr.terms[0].sum_ref->attribute == "flow");
    CHECK(expr.terms[0].sum_ref->element_ids.size() == 2);
    CHECK(expr.terms[0].sum_ref->element_ids[0] == "L1");
    CHECK(expr.terms[0].sum_ref->element_ids[1] == "L2");
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Parse line.flowp and line.flown remain single terms")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // Explicit directional references must NOT be rewritten — only `flow`
    // triggers the expansion.
    auto flowp_expr = ConstraintParser::parse(R"(line("L1").flowp <= 100)");
    REQUIRE(flowp_expr.terms.size() == 1);
    REQUIRE(flowp_expr.terms[0].element.has_value());
    CHECK(flowp_expr.terms[0].element->attribute == "flowp");

    auto flown_expr = ConstraintParser::parse(R"(line("L1").flown <= 100)");
    REQUIRE(flown_expr.terms.size() == 1);
    REQUIRE(flown_expr.terms[0].element.has_value());
    CHECK(flown_expr.terms[0].element->attribute == "flown");
  }

  TEST_CASE("Parse battery energy constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(battery("BESS1").energy <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "battery");
    CHECK(ref0.element_id == "BESS1");
    CHECK(ref0.attribute == "energy");
  }

  TEST_CASE("Parse demand fail constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(demand("D1").fail <= 10)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "demand");
    CHECK(ref0.attribute == "fail");
  }

  TEST_CASE("Parse demand load and fail in same expression")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(demand("D1").load + demand("D1").fail = 100)");

    CHECK(expr.constraint_type == ConstraintType::EQUAL);
    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.attribute == "load");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.attribute == "fail");
  }

  // ── UID references ─────────────────────────────────────────────────────

  TEST_CASE("Parse mixed name and uid references")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("TORO").generation + demand("uid:10").load <= 500)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "TORO");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_id == "uid:10");
    CHECK(ref1.element_type == "demand");
  }

  // ── Multi-term expressions ─────────────────────────────────────────────

  TEST_CASE("Parse three-element sum constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation + generator("G2").generation + generator("G3").generation <= 500)");

    REQUIRE(expr.terms.size() == 3);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_id == "G2");
    REQUIRE(expr.terms[2].element.has_value());
    const auto& ref2 = expr.terms[2].element.value_or(ElementRef {});
    CHECK(ref2.element_id == "G3");
  }

  TEST_CASE("Parse mixed-type multi-element expression")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(2 * generator("G1").generation - demand("D1").load + line("L1").flow >= 0)");

    CHECK(expr.constraint_type == ConstraintType::GREATER_EQUAL);
    // `line.flow` is preserved in the AST as a compound reference; its
    // expansion into (+flowp - flown) now happens at row-assembly time.
    REQUIRE(expr.terms.size() == 3);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));
    CHECK(ref0.element_type == "generator");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(expr.terms[1].coefficient == doctest::Approx(-1.0));
    CHECK(ref1.element_type == "demand");
    REQUIRE(expr.terms[2].element.has_value());
    const auto& ref2 = expr.terms[2].element.value_or(ElementRef {});
    CHECK(expr.terms[2].coefficient == doctest::Approx(1.0));
    CHECK(ref2.element_type == "line");
    CHECK(ref2.attribute == "flow");
  }

  // ── Variables on both sides ────────────────────────────────────────────

  TEST_CASE("Parse with variables on both sides of >=")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation >= demand("D1").load)");

    CHECK(expr.constraint_type == ConstraintType::GREATER_EQUAL);
    CHECK(expr.rhs == doctest::Approx(0.0));
    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(expr.terms[1].coefficient == doctest::Approx(-1.0));
    CHECK(ref1.element_id == "D1");
  }

  TEST_CASE("Parse constraint with constant on both sides")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(generator("G1").generation + 10 <= 50)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    // 50 - 10 = 40
    CHECK(expr.rhs == doctest::Approx(40.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
  }

  // ── Whitespace and formatting ──────────────────────────────────────────

  TEST_CASE("Parse with extra whitespace")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(   generator( "G1" ) . generation   <=   100   )");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
  }

  TEST_CASE("Parse with no whitespace")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(generator("G1").generation<=100)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
  }

  // ── Named constraint ───────────────────────────────────────────────────

  TEST_CASE("Parse with named constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse("my_limit",
                                        R"(generator("G1").generation <= 100)");

    CHECK(expr.name == "my_limit");
  }

  // ── Error paths ────────────────────────────────────────────────────────

  TEST_CASE("Error: empty expression")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse("")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: missing constraint operator")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1").generation 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: invalid element type")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(foo("G1").generation <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: unterminated string")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1).generation <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: invalid for clause dimension")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(week in {1}))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: missing attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(R"(generator("G1") <= 100)")),
        std::invalid_argument);
  }

  TEST_CASE("Error: non-linear product of two element references")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // Product of two decision variables is bilinear and must be rejected.
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator('G1').generation *
                           generator('G2').generation <= 100)")),
                    std::invalid_argument);
  }

  // ── Bare numeric UID identification ──────────────────────────────────

  TEST_CASE("Parse generator with bare numeric UID")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(generator(3).generation <= 200)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(200.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "generator");
    CHECK(ref0.element_id == "uid:3");
    CHECK(ref0.attribute == "generation");
  }

  TEST_CASE("Parse demand with bare numeric UID")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(demand(7).load >= 50)");

    CHECK(expr.constraint_type == ConstraintType::GREATER_EQUAL);
    CHECK(expr.rhs == doctest::Approx(50.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "demand");
    CHECK(ref0.element_id == "uid:7");
    CHECK(ref0.attribute == "load");
  }

  TEST_CASE("Parse mixed name and numeric UID references")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation + generator(5).generation <= 300)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_id == "uid:5");
  }

  TEST_CASE("Parse coefficient with numeric UID")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(2.5 * generator(1).generation <= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "uid:1");
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.5));
  }

  // ── New element types ─────────────────────────────────────────────────

  TEST_CASE("Parse converter element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(converter("CV1").charge <= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "converter");
    CHECK(ref0.element_id == "CV1");
    CHECK(ref0.attribute == "charge");
  }

  TEST_CASE("Parse reservoir element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(reservoir("RES1").volume >= 1000)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reservoir");
    CHECK(ref0.element_id == "RES1");
    CHECK(ref0.attribute == "volume");
  }

  TEST_CASE("Parse bus element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(bus("B1").theta <= 3.14)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "bus");
    CHECK(ref0.element_id == "B1");
    CHECK(ref0.attribute == "theta");
  }

  TEST_CASE("Parse waterway element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(waterway(2).flow <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "waterway");
    CHECK(ref0.element_id == "uid:2");
    CHECK(ref0.attribute == "flow");
  }

  TEST_CASE("Parse turbine element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(turbine("T1").generation <= 80)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "turbine");
    CHECK(ref0.element_id == "T1");
    CHECK(ref0.attribute == "generation");
  }

  // ── Hydro elements ────────────────────────────────────────────────────

  TEST_CASE("Parse junction drain element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(junction("J1").drain <= 1000)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "junction");
    CHECK(ref0.element_id == "J1");
    CHECK(ref0.attribute == "drain");
  }

  TEST_CASE("Parse flow discharge element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(flow("F1").discharge >= 10)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "flow");
    CHECK(ref0.element_id == "F1");
    CHECK(ref0.attribute == "discharge");
  }

  TEST_CASE("Parse flow element with numeric UID")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(flow(3).flow <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "flow");
    CHECK(ref0.element_id == "uid:3");
    CHECK(ref0.attribute == "flow");
  }

  TEST_CASE("Parse seepage element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(seepage("FIL1").flow <= 200)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "seepage");
    CHECK(ref0.element_id == "FIL1");
    CHECK(ref0.attribute == "flow");
  }

  TEST_CASE("Parse reserve_provision element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(reserve_provision("RP1").up <= 50)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reserve_provision");
    CHECK(ref0.element_id == "RP1");
    CHECK(ref0.attribute == "up");
  }

  TEST_CASE("Parse reserve_zone element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(reserve_zone("RZ1").dn >= 10)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reserve_zone");
    CHECK(ref0.element_id == "RZ1");
    CHECK(ref0.attribute == "dn");
  }

  TEST_CASE("Parse cross-element hydro constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(waterway("WW1").flow + junction("J1").drain <= 500)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "waterway");
    CHECK(ref0.attribute == "flow");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_type == "junction");
    CHECK(ref1.attribute == "drain");
  }

  TEST_CASE("Parse sum over all flows")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(sum(flow(all).flow) <= 1000)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sref0.element_type == "flow");
    CHECK(sref0.all_elements);
    CHECK(sref0.attribute == "flow");
  }

  TEST_CASE("Parse sum over explicit junctions")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(sum(junction("J1","J2").drain) <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sref0.element_type == "junction");
    CHECK_FALSE(sref0.all_elements);
    REQUIRE(sref0.element_ids.size() == 2);
    CHECK(sref0.element_ids[0] == "J1");
    CHECK(sref0.element_ids[1] == "J2");
    CHECK(sref0.attribute == "drain");
  }

  // ── Extended attributes ───────────────────────────────────────────────

  TEST_CASE("Parse line with flowp attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(line("L1").flowp <= 150)");

    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.attribute == "flowp");
  }

  TEST_CASE("Parse line with lossp attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(line("L1").lossp <= 10)");

    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.attribute == "lossp");
  }

  TEST_CASE("Parse line with lossn attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(line("L1").lossn <= 10)");

    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.attribute == "lossn");
  }

  TEST_CASE("Parse battery with charge and discharge attributes")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(battery("B1").charge + battery("B1").discharge <= 100)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref0.attribute == "charge");
    CHECK(ref1.attribute == "discharge");
  }

  // ── sum() aggregation ─────────────────────────────────────────────────

  TEST_CASE("Parse sum with explicit element list")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator("G1","G2","G3").generation) <= 500)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(500.0));
    REQUIRE(expr.terms.size() == 1);
    CHECK_FALSE(expr.terms[0].element.has_value());
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sref0.element_type == "generator");
    CHECK_FALSE(sref0.all_elements);
    REQUIRE(sref0.element_ids.size() == 3);
    CHECK(sref0.element_ids[0] == "G1");
    CHECK(sref0.element_ids[1] == "G2");
    CHECK(sref0.element_ids[2] == "G3");
    CHECK(sref0.attribute == "generation");
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Parse sum over all generators")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(sum(generator(all).generation) <= 1000)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sref0.element_type == "generator");
    CHECK(sref0.all_elements);
    CHECK(sref0.element_ids.empty());
    CHECK(sref0.attribute == "generation");
  }

  TEST_CASE("Parse sum with coefficient")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(0.5 * sum(demand("D1","D2").load) <= 200)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(expr.terms[0].coefficient == doctest::Approx(0.5));
    CHECK(sref0.element_type == "demand");
    REQUIRE(sref0.element_ids.size() == 2);
    CHECK(sref0.attribute == "load");
  }

  TEST_CASE("Parse sum with numeric UIDs")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(sum(generator(1, 2, 3).generation) <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    REQUIRE(sref0.element_ids.size() == 3);
    CHECK(sref0.element_ids[0] == "uid:1");
    CHECK(sref0.element_ids[1] == "uid:2");
    CHECK(sref0.element_ids[2] == "uid:3");
  }

  TEST_CASE("Parse sum with mixed name and numeric UIDs")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator("G1", 2, "uid:3").generation) <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    REQUIRE(sref0.element_ids.size() == 3);
    CHECK(sref0.element_ids[0] == "G1");
    CHECK(sref0.element_ids[1] == "uid:2");
    CHECK(sref0.element_ids[2] == "uid:3");
  }

  TEST_CASE("Parse sum plus single element")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator("G1","G2").generation) + demand("D1").load <= 1000)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sref0.element_type == "generator");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_type == "demand");
  }

  TEST_CASE("Parse sum subtraction")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all).generation) - sum(demand(all).load) = 0)");

    CHECK(expr.constraint_type == ConstraintType::EQUAL);
    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
    REQUIRE(expr.terms[1].sum_ref.has_value());
    CHECK(expr.terms[1].coefficient == doctest::Approx(-1.0));
  }

  TEST_CASE("Parse sum with domain clause")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all).generation) <= 500, for(block in 1..12))");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sref0 = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sref0.all_elements);
    CHECK_FALSE(expr.domain.blocks.is_all);
    REQUIRE(expr.domain.blocks.values.size() == 12);
    CHECK(expr.domain.blocks.values[0] == 1);
    CHECK(expr.domain.blocks.values[11] == 12);
  }

  // ── Comment support ───────────────────────────────────────────────────

  TEST_CASE("Parse expression with hash comment")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        "generator(\"G1\").generation <= 100 # limit gen output");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
  }

  TEST_CASE("Parse expression with double-slash comment")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        "generator(\"G1\").generation <= 100 // limit gen output");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
  }

  TEST_CASE("Parse multiline expression with comments")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        "# Limit total generation\n"
        "generator(\"G1\").generation  # first gen\n"
        "+ generator(\"G2\").generation  // second gen\n"
        "<= 300");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(300.0));
    REQUIRE(expr.terms.size() == 2);
  }

  // ── Cross-element constraints with new types ──────────────────────────

  TEST_CASE("Parse cross-element constraint with converter and battery")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(converter("CV1").discharge - battery("B1").energy <= 0)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "converter");
    CHECK(ref0.attribute == "discharge");
    CHECK(ref1.element_type == "battery");
    CHECK(ref1.attribute == "energy");
    // RHS term moved to LHS with negation
    CHECK(expr.terms[1].coefficient == doctest::Approx(-1.0));
  }

  TEST_CASE("Parse range constraint with numeric UID")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(50 <= generator(1).generation <= 250)");

    CHECK(expr.constraint_type == ConstraintType::RANGE);
    REQUIRE(expr.lower_bound.has_value());
    REQUIRE(expr.upper_bound.has_value());
    CHECK(expr.lower_bound.value_or(0.0) == doctest::Approx(50.0));
    CHECK(expr.upper_bound.value_or(0.0) == doctest::Approx(250.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "uid:1");
  }

  // ── Single-quote string literals ──────────────────────────────────────────

  TEST_CASE("Single-quote element id is accepted")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // generator('G1') must parse identically to generator("G1")
    auto expr = ConstraintParser::parse(R"(generator('G1').generation <= 100)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "generator");
    CHECK(ref0.element_id == "G1");
    CHECK(ref0.attribute == "generation");
  }

  TEST_CASE("Mixed single- and double-quote element ids")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(generator('G1').generation + generator("G2").generation <= 300)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G1");
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref1.element_id == "G2");
  }

  TEST_CASE("Single-quote uid: reference")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(generator('uid:5').generation <= 200)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "uid:5");
  }

  // ── type filter in sum() (legacy `type="..."` form) ───────────────────────

  TEST_CASE("sum(all, type=) parses legacy type filter into filters vector")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all, type="hydro").generation) <= 500)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(500.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.element_type == "generator");
    CHECK(sr.all_elements == true);
    REQUIRE(sr.filters.size() == 1);
    CHECK(sr.filters[0].attr == "type");
    CHECK(sr.filters[0].op == SumPredicate::Op::Eq);
    CHECK(sr.filters[0].string_value.value_or("") == "hydro");
    CHECK(sr.attribute == "generation");
  }

  TEST_CASE("sum(all) without filters has empty filters vector")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(sum(generator(all).generation) <= 1000)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.all_elements == true);
    CHECK(sr.filters.empty());
  }

  TEST_CASE("sum(all, type=) with single-quote type value")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(demand(all, type='industrial').load) >= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.element_type == "demand");
    CHECK(sr.all_elements == true);
    REQUIRE(sr.filters.size() == 1);
    CHECK(sr.filters[0].attr == "type");
    CHECK(sr.filters[0].string_value.value_or("") == "industrial");
    CHECK(sr.attribute == "load");
  }

  // ── F4: multi-predicate sum filters (`sum(... : pred and pred)`) ───────

  TEST_CASE("F4: sum with single predicate after colon")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all : type="hydro").generation) <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.element_type == "generator");
    CHECK(sr.all_elements == true);
    REQUIRE(sr.filters.size() == 1);
    CHECK(sr.filters[0].attr == "type");
    CHECK(sr.filters[0].op == SumPredicate::Op::Eq);
    CHECK(sr.filters[0].string_value.value_or("") == "hydro");
    CHECK(sr.attribute == "generation");
  }

  TEST_CASE("F4: sum with conjunction of predicates (and)")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all : type="hydro" and bus=2).generation) <= 500)");

    REQUIRE(expr.terms.size() == 1);
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    REQUIRE(sr.filters.size() == 2);
    CHECK(sr.filters[0].attr == "type");
    CHECK(sr.filters[0].op == SumPredicate::Op::Eq);
    CHECK(sr.filters[0].string_value.value_or("") == "hydro");
    CHECK(sr.filters[1].attr == "bus");
    CHECK(sr.filters[1].op == SumPredicate::Op::Eq);
    CHECK(sr.filters[1].number_value.value_or(-1.0) == doctest::Approx(2.0));
  }

  TEST_CASE("F4: sum predicate with != operator")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(line(all : type != "dc").flowp) <= 1000)");

    REQUIRE(expr.terms.size() == 1);
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    REQUIRE(sr.filters.size() == 1);
    CHECK(sr.filters[0].attr == "type");
    CHECK(sr.filters[0].op == SumPredicate::Op::Ne);
    CHECK(sr.filters[0].string_value.value_or("") == "dc");
  }

  TEST_CASE("F4: sum predicate with numeric comparison")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all : bus >= 10).generation) <= 300)");

    REQUIRE(expr.terms.size() == 1);
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    REQUIRE(sr.filters.size() == 1);
    CHECK(sr.filters[0].attr == "bus");
    CHECK(sr.filters[0].op == SumPredicate::Op::Ge);
    CHECK(sr.filters[0].number_value.value_or(-1.0) == doctest::Approx(10.0));
  }

  TEST_CASE("F4: sum predicate with `in` set membership")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(sum(generator(all : type in {"hydro","solar"}).generation) <= 500)");

    REQUIRE(expr.terms.size() == 1);
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    REQUIRE(sr.filters.size() == 1);
    CHECK(sr.filters[0].attr == "type");
    CHECK(sr.filters[0].op == SumPredicate::Op::In);
    REQUIRE(sr.filters[0].set_values.size() == 2);
    CHECK(sr.filters[0].set_values[0] == "hydro");
    CHECK(sr.filters[0].set_values[1] == "solar");
  }

  // ── New storage attributes: spill / drain / extraction ────────────────

  TEST_CASE("Parse battery spill attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(battery("B1").spill <= 10)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "battery");
    CHECK(ref0.element_id == "B1");
    CHECK(ref0.attribute == "spill");
  }

  TEST_CASE("Parse battery drain alias")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(battery("B1").drain <= 5)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "battery");
    CHECK(ref0.attribute == "drain");
  }

  TEST_CASE("Parse reservoir spill attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(reservoir("RES1").spill <= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reservoir");
    CHECK(ref0.element_id == "RES1");
    CHECK(ref0.attribute == "spill");
  }

  TEST_CASE("Parse reservoir drain alias")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(R"(reservoir("RES1").drain <= 50)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reservoir");
    CHECK(ref0.attribute == "drain");
  }

  TEST_CASE("Parse reservoir extraction attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(reservoir("RES1").extraction <= 200)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reservoir");
    CHECK(ref0.element_id == "RES1");
    CHECK(ref0.attribute == "extraction");
  }

  TEST_CASE("Parse cross-element with reservoir extraction and battery spill")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr = ConstraintParser::parse(
        R"(reservoir("R1").extraction + battery("B1").spill <= 300)");

    REQUIRE(expr.terms.size() == 2);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    REQUIRE(expr.terms[1].element.has_value());
    const auto& ref1 = expr.terms[1].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reservoir");
    CHECK(ref0.attribute == "extraction");
    CHECK(ref1.element_type == "battery");
    CHECK(ref1.attribute == "spill");
  }

  // ── Additional error paths ──────────────────────────────────────────────

  TEST_CASE("Error: unexpected character in expression")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1").generation @ 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: range constraint with non-constant lower bound")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= generator("G2").generation <= 100)")),
        std::invalid_argument);
  }

  TEST_CASE("Error: range constraint with non-constant upper bound")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(10 <= generator("G1").generation <= demand("D1").load)")),
        std::invalid_argument);
  }

  TEST_CASE("Error: missing 'in' or '=' after dimension in for clause")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1").generation <= 100, for(stage 1))")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-element type after comma in sum(all,...)")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator(all, foo).generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-string type value in sum(all, type=)")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator(all, type=123).generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: unexpected token in sum element list")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator(<=).generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-element type in sum()")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(foo("G1").generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-ident dimension in for clause")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(123 in all))")),
        std::invalid_argument);
  }

  TEST_CASE("Lexer peek does not consume token")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // Verifying peek() by parsing a valid expression where peek is exercised
    // This expression triggers the single-value parsing path (no braces)
    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(block in 5))");

    CHECK_FALSE(expr.domain.blocks.is_all);
    REQUIRE(expr.domain.blocks.values.size() == 1);
    CHECK(expr.domain.blocks.values[0] == 5);
  }

  TEST_CASE("Parse for clause without comma separator")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // 'for' directly after expression (no comma)
    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100 for(stage in 1..3))");

    CHECK_FALSE(expr.domain.stages.is_all);
    REQUIRE(expr.domain.stages.values.size() == 3);
  }

  TEST_CASE("Parse with escape character in string literal")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(generator("G\"1").generation <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G\"1");
  }

  TEST_CASE("Parse with leading plus sign")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    auto expr =
        ConstraintParser::parse(R"(+generator("G1").generation <= 100)");
    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Error: bad element id type (not string or number)")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator(<=).generation <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: missing attribute after dot")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(
                        ConstraintParser::parse(R"(generator("G1"). <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: missing attribute after dot in sum")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator("G1"). ) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: expected number after .. in index set")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in 1..foo))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: expected number after .. in braced index set")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in {1..foo}))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: non-number in braced index set")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in {foo}))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: invalid index set starting token")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in <=))")),
        std::invalid_argument);
  }

  // ── Named parameter references ──────────────────────────────────────────

  TEST_CASE("Bare parameter name on RHS")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        "param_rhs", R"(generator("G1").generation <= my_limit)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    // The param reference ends up as a term on the RHS, moved to LHS negated
    // LHS: generator.generation, RHS: my_limit
    // After normalization: generator.generation - my_limit <= 0
    // So we should have variable term + param_name term
    bool found_var = false;
    bool found_param = false;
    for (const auto& term : expr.terms) {
      if (term.element.has_value()) {
        found_var = true;
      }
      if (term.param_name.has_value()) {
        CHECK(*term.param_name == "my_limit");
        CHECK(term.coefficient == doctest::Approx(-1.0));
        found_param = true;
      }
    }
    CHECK(found_var);
    CHECK(found_param);
  }

  TEST_CASE("Parameter name with coefficient")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        "param_coeff", R"(generator("G1").generation <= 2 * scale_factor)");

    bool found_param = false;
    for (const auto& term : expr.terms) {
      if (term.param_name.has_value() && *term.param_name == "scale_factor") {
        CHECK(term.coefficient == doctest::Approx(-2.0));
        found_param = true;
      }
    }
    CHECK(found_param);
  }

  TEST_CASE("Parameter name on LHS")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        "param_lhs", R"(generator("G1").generation + offset <= 100)");

    bool found_param = false;
    for (const auto& term : expr.terms) {
      if (term.param_name.has_value() && *term.param_name == "offset") {
        found_param = true;
      }
    }
    CHECK(found_param);
  }

  // ── F1: Parenthesized subexpressions ─────────────────────────────────

  TEST_CASE("Parens: simple grouping scales both terms")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(2 * (generator('G1').generation + generator('G2').generation)
           <= 300)");
    REQUIRE(expr.terms.size() == 2);
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));
    CHECK(expr.terms[1].coefficient == doctest::Approx(2.0));
    CHECK(expr.rhs == doctest::Approx(300.0));
  }

  TEST_CASE("Parens: group on RHS is also scaled")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(generator('G1').generation
           <= 0.5 * (demand('D1').load + demand('D2').load))");
    REQUIRE(expr.terms.size() == 3);
    // G1 stays on LHS; D1 and D2 are moved to LHS with negated scaled coeffs.
    for (const auto& t : expr.terms) {
      if (t.element && t.element->element_type == "generator") {
        CHECK(t.coefficient == doctest::Approx(1.0));
      } else if (t.element && t.element->element_type == "demand") {
        CHECK(t.coefficient == doctest::Approx(-0.5));
      }
    }
  }

  TEST_CASE("Parens: negated group distributes sign")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(-(generator('G1').generation - generator('G2').generation) <= 50)");
    REQUIRE(expr.terms.size() == 2);
    for (const auto& t : expr.terms) {
      REQUIRE(t.element.has_value());
      if (t.element->element_id == "G1") {
        CHECK(t.coefficient == doctest::Approx(-1.0));
      } else {
        CHECK(t.coefficient == doctest::Approx(1.0));
      }
    }
  }

  TEST_CASE("Parens: nested parens fold correctly")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(2 * (3 * (generator('G1').generation)) <= 120)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(6.0));
    CHECK(expr.rhs == doctest::Approx(120.0));
  }

  TEST_CASE("Parens: group containing sum")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(2 * (sum(generator(all).generation) + demand('D1').load) <= 1000)");
    // Expect one sum term (coef 2) and one load term (coef 2).
    REQUIRE(expr.terms.size() == 2);
    bool seen_sum = false;
    bool seen_load = false;
    for (const auto& t : expr.terms) {
      if (t.sum_ref.has_value()) {
        CHECK(t.coefficient == doctest::Approx(2.0));
        seen_sum = true;
      } else if (t.element.has_value()) {
        CHECK(t.coefficient == doctest::Approx(2.0));
        seen_load = true;
      }
    }
    CHECK(seen_sum);
    CHECK(seen_load);
  }

  TEST_CASE("Parens: group with line.flow preserves compound attribute")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(2 * (line('L1').flow) <= 100)");
    // `line.flow` stays as a single compound term; the outer coefficient
    // from the paren group is applied directly to it.
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    CHECK(expr.terms[0].element->element_type == "line");
    CHECK(expr.terms[0].element->attribute == "flow");
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));
  }

  TEST_CASE("Parens: mismatched parens rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(2 * (generator('G1').generation <= 100)")),
                    std::invalid_argument);
  }

  // ── F2: Division by a constant ───────────────────────────────────────

  TEST_CASE("Division: LHS divided by literal")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(generator('G1').generation / 2 <= 50)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(0.5));
    CHECK(expr.rhs == doctest::Approx(50.0));
  }

  TEST_CASE("Division: parenthesized sum divided by literal")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"((generator('G1').generation + generator('G2').generation) / 4
           <= 25)");
    REQUIRE(expr.terms.size() == 2);
    for (const auto& t : expr.terms) {
      CHECK(t.coefficient == doctest::Approx(0.25));
    }
  }

  TEST_CASE("Division: folds with constant expression divisor")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(generator('G1').generation / (2 + 2) <= 10)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(0.25));
  }

  TEST_CASE("Division: by zero rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator('G1').generation / 0 <= 10)")),
                    std::invalid_argument);
  }

  TEST_CASE("Division: by variable rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator('G1').generation /
                           generator('G2').generation <= 10)")),
                    std::invalid_argument);
  }

  // ── F3: Constant folding in coefficients ─────────────────────────────

  TEST_CASE("Constant folding: (2+3) * x yields coefficient 5")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"((2 + 3) * generator('G1').generation <= 100)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(5.0));
  }

  TEST_CASE("Constant folding: chained multiplication 2 * 3 * x")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(2 * 3 * generator('G1').generation <= 120)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(6.0));
  }

  TEST_CASE("Constant folding: coefficient after variable is legal")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(generator('G1').generation * 2 <= 100)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));
  }

  TEST_CASE("Constant folding: pure-constant LHS yields trivial constraint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // 2 * 3 <= 100 folds to 6 <= 100 — a trivially-true constraint with
    // no variable terms.  The parser accepts it and emits zero terms.
    const auto expr = ConstraintParser::parse(R"(2 * 3 <= 100)");
    CHECK(expr.terms.empty());
    CHECK(expr.rhs == doctest::Approx(94.0));
  }

  TEST_CASE("Constant folding: mixed precedence with addition")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // 10 + 2 * generator('G1').generation:
    //   constant 10 absorbed into RHS, variable term has coef 2.
    const auto expr = ConstraintParser::parse(
        R"(10 + 2 * generator('G1').generation <= 130)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));
    CHECK(expr.rhs == doctest::Approx(120.0));
  }

  TEST_CASE("Constant folding: division inside coefficient")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"((6 / 2) * generator('G1').generation <= 100)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(3.0));
  }

  // ── F10 diagnostics: caret + hint format ──────────────────────────────

  TEST_CASE("Diagnostics: ConstraintParseError is-a invalid_argument")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    // Backward compatibility: existing CHECK_THROWS_AS(..., invalid_argument)
    // sites must keep working.
    CHECK_THROWS_AS(static_cast<void>(
                        ConstraintParser::parse("generator('G1').generation")),
                    std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(
                        ConstraintParser::parse("generator('G1').generation")),
                    ConstraintParseError);
  }

  TEST_CASE("Diagnostics: error message includes 'Parse error at column N'")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    try {
      [[maybe_unused]] auto expr = ConstraintParser::parse(
          R"(generator('G1').generation * generator('G2').generation <= 100)");
      FAIL("expected ConstraintParseError");
    } catch (const ConstraintParseError& e) {
      const std::string what = e.what();
      CHECK(what.find("Parse error at column") != std::string::npos);
      // Column points inside the source.
      CHECK(e.column() < what.size());
      // Error body mentions nonlinearity.
      CHECK(e.message().find("Non-linear") != std::string::npos);
    }
  }

  TEST_CASE("Diagnostics: division by zero carries a hint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    try {
      [[maybe_unused]] auto expr =
          ConstraintParser::parse(R"(generator('G1').generation / 0 <= 100)");
      FAIL("expected ConstraintParseError");
    } catch (const ConstraintParseError& e) {
      // Hint explains what a valid divisor looks like.
      CHECK(!e.hint().empty());
      CHECK(e.hint().find("numeric constant") != std::string::npos);
    }
  }

  TEST_CASE("Diagnostics: division by variable carries a hint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    try {
      [[maybe_unused]] auto expr = ConstraintParser::parse(
          R"(generator('G1').generation / generator('G2').generation <= 100)");
      FAIL("expected ConstraintParseError");
    } catch (const ConstraintParseError& e) {
      CHECK(!e.hint().empty());
      CHECK(e.hint().find("numeric constant") != std::string::npos);
    }
  }

  TEST_CASE("Diagnostics: caret pointer is rendered in formatted message")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    try {
      [[maybe_unused]] auto expr =
          ConstraintParser::parse(R"(generator('G1').generation @ 100)");
      FAIL("expected ConstraintParseError");
    } catch (const ConstraintParseError& e) {
      const std::string what = e.what();
      // Must contain a caret line.
      CHECK(what.find('^') != std::string::npos);
      // And the original source.
      CHECK(what.find("@ 100") != std::string::npos);
    }
  }

  TEST_CASE("Diagnostics: missing attribute after dot has hint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    try {
      [[maybe_unused]] auto expr =
          ConstraintParser::parse(R"(generator('G1'). <= 100)");
      FAIL("expected ConstraintParseError");
    } catch (const ConstraintParseError& e) {
      CHECK(!e.hint().empty());
    }
  }

  TEST_CASE("Diagnostics: nonlinear product has explanatory hint")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    try {
      [[maybe_unused]] auto expr = ConstraintParser::parse(
          R"(generator('G1').generation * generator('G2').generation <= 100)");
      FAIL("expected ConstraintParseError");
    } catch (const ConstraintParseError& e) {
      CHECK(!e.hint().empty());
      // Hint explicitly calls out that '*' needs a constant operand.
      CHECK(e.hint().find("constant") != std::string::npos);
    }
  }

  // ── F5: abs(x) auto-linearization (parse) ─────────────────────────────

  TEST_CASE("F5: parse abs(generator) <= rhs produces abs_expr term")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(abs(generator('G1').generation) <= 50)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].abs_expr);
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(50.0));
    REQUIRE(expr.terms[0].abs_expr->inner.size() == 1);
    REQUIRE(expr.terms[0].abs_expr->inner[0].element.has_value());
    CHECK(expr.terms[0].abs_expr->inner[0].element->element_type
          == "generator");
    CHECK(expr.terms[0].abs_expr->inner[0].element->attribute == "generation");
  }

  TEST_CASE("F5: parse coeff * abs(linear) preserves outer coefficient")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(2 * abs(generator('G1').generation - 10) <= 40)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].abs_expr);
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));
    // Inner expression has the generator term and the -10 constant.
    REQUIRE(expr.terms[0].abs_expr->inner.size() == 2);
  }

  TEST_CASE("F5: abs(constant) folds to |constant|")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(abs(-7) + generator('G').generation <= 20)");
    // The folded |−7| = 7 becomes a pure constant term absorbed into RHS.
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].element.has_value());
    CHECK(expr.rhs == doctest::Approx(13.0));
  }

  TEST_CASE("F5: nested abs(abs(...)) is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(abs(abs(generator('G1').generation)) <= 10)")),
                    std::invalid_argument);
  }

  // ── F7: min/max(arg1, arg2, ...) auto-linearization (parse) ────────────

  TEST_CASE("F7: parse max with two element arguments")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(max(generator('G1').generation,
               generator('G2').generation) <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].minmax_expr);
    CHECK(expr.terms[0].minmax_expr->kind == MinMaxKind::Max);
    CHECK(expr.terms[0].minmax_expr->args.size() == 2);
  }

  TEST_CASE("F7: parse min with three arguments")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(min(generator('G1').generation,
               generator('G2').generation,
               generator('G3').generation) >= 10)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].minmax_expr);
    CHECK(expr.terms[0].minmax_expr->kind == MinMaxKind::Min);
    CHECK(expr.terms[0].minmax_expr->args.size() == 3);
  }

  TEST_CASE("F7: max with only one arg is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(max(generator('G1').generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("F7: max(constants) folds at parse time")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(max(3, 5, 1) + generator('G').generation <= 20)");
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].element.has_value());
    // RHS = 20 − 5 (folded max).
    CHECK(expr.rhs == doctest::Approx(15.0));
  }

  // ── F8: if-then-else data-only conditional (parse) ─────────────────────

  TEST_CASE("F8: parse if-then on stage coordinate")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(if stage = 2 then (generator('G1').generation) <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].if_expr);
    REQUIRE(expr.terms[0].if_expr->cond.size() == 1);
    CHECK(expr.terms[0].if_expr->cond[0].coord == IfCondAtom::Coord::Stage);
    CHECK(expr.terms[0].if_expr->cond[0].op == IfCondAtom::Op::Eq);
    CHECK(expr.terms[0].if_expr->cond[0].number.value_or(-1) == 2);
    CHECK(expr.terms[0].if_expr->then_branch.size() == 1);
    CHECK(expr.terms[0].if_expr->else_branch.empty());
  }

  TEST_CASE("F8: parse if-then-else on block in { … }")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(if block in {1, 2, 3} then (generator('G1').generation)
           else (generator('G2').generation) <= 80)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].if_expr);
    REQUIRE(expr.terms[0].if_expr->cond.size() == 1);
    CHECK(expr.terms[0].if_expr->cond[0].coord == IfCondAtom::Coord::Block);
    CHECK(expr.terms[0].if_expr->cond[0].op == IfCondAtom::Op::In);
    CHECK(expr.terms[0].if_expr->cond[0].set_values.size() == 3);
    CHECK(expr.terms[0].if_expr->then_branch.size() == 1);
    CHECK(expr.terms[0].if_expr->else_branch.size() == 1);
  }

  TEST_CASE("F8: if-condition conjunction with 'and'")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(if stage >= 2 and scenario = 1 then
           (generator('G1').generation) <= 50)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].if_expr);
    CHECK(expr.terms[0].if_expr->cond.size() == 2);
    CHECK(expr.terms[0].if_expr->cond[0].coord == IfCondAtom::Coord::Stage);
    CHECK(expr.terms[0].if_expr->cond[0].op == IfCondAtom::Op::Ge);
    CHECK(expr.terms[0].if_expr->cond[1].coord == IfCondAtom::Coord::Scenario);
  }

  TEST_CASE("F8: if without then is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(if stage = 1 (generator('G').generation) <= 10)")),
                    std::invalid_argument);
  }

  // ── Phase 1d: singleton-class scalars ──────────────────────────────────

  TEST_CASE("Singleton class scalar: options.scale_objective")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(options.scale_objective <= 1000)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref.element_type == "options");
    CHECK(ref.element_id.empty());
    CHECK(ref.attribute == "scale_objective");
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Singleton class scalar with coefficient")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(2.5 * options.annual_discount_rate + generator("G1").generation <= 100)");
    REQUIRE(expr.terms.size() == 2);

    REQUIRE(expr.terms[0].element.has_value());
    const auto& opt_ref = expr.terms[0].element.value_or(ElementRef {});
    CHECK(opt_ref.element_type == "options");
    CHECK(opt_ref.element_id.empty());
    CHECK(opt_ref.attribute == "annual_discount_rate");
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.5));

    REQUIRE(expr.terms[1].element.has_value());
    const auto& gen_ref = expr.terms[1].element.value_or(ElementRef {});
    CHECK(gen_ref.element_type == "generator");
    CHECK(gen_ref.element_id == "G1");
  }

  TEST_CASE("Singleton class without dot is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(R"(options <= 100)")),
        std::invalid_argument);
  }

  TEST_CASE("Singleton class with dot but no attribute is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(R"(options. <= 100)")),
        std::invalid_argument);
  }

  // ── Phase 1e: state(...) wrapper grammar ───────────────────────────────

  TEST_CASE("Phase 1e: state(reservoir(...).efin) parses with state_wrapped")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr =
        ConstraintParser::parse(R"(state(reservoir("R1").efin) <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref.element_type == "reservoir");
    CHECK(ref.element_id == "R1");
    CHECK(ref.attribute == "efin");
    CHECK(ref.state_wrapped);
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Phase 1e: bare element ref keeps state_wrapped == false")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(R"(reservoir("R1").efin <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref = expr.terms[0].element.value_or(ElementRef {});
    CHECK_FALSE(ref.state_wrapped);
  }

  TEST_CASE("Phase 1e: state(...) participates in linear combinations")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    const auto expr = ConstraintParser::parse(
        R"(2 * state(reservoir("R1").efin) - reservoir("R2").eini <= 50)");
    REQUIRE(expr.terms.size() == 2);

    REQUIRE(expr.terms[0].element.has_value());
    const auto& wrapped = expr.terms[0].element.value_or(ElementRef {});
    CHECK(wrapped.state_wrapped);
    CHECK(wrapped.element_id == "R1");
    CHECK(expr.terms[0].coefficient == doctest::Approx(2.0));

    REQUIRE(expr.terms[1].element.has_value());
    const auto& bare = expr.terms[1].element.value_or(ElementRef {});
    CHECK_FALSE(bare.state_wrapped);
    CHECK(bare.element_id == "R2");
    CHECK(expr.terms[1].coefficient == doctest::Approx(-1.0));
  }

  TEST_CASE("Phase 1e: nested state(state(...)) is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(state(state(reservoir("R1").efin)) <= 0)")),
                    std::invalid_argument);
  }

  TEST_CASE("Phase 1e: state(<numeric literal>) is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(R"(state(42) <= 0)")),
        std::invalid_argument);
  }

  TEST_CASE("Phase 1e: state(sum(...)) is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(state(sum(reservoir(all).efin)) <= 0)")),
                    std::invalid_argument);
  }

  TEST_CASE("Phase 1e: state without parens is rejected")
  {
    using namespace gtopt;  // NOLINT(google-build-using-namespace)

    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(state reservoir("R1").efin <= 0)")),
                    std::invalid_argument);
  }
}
