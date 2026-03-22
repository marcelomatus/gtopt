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
    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(stage in all, block in all))");

    CHECK(expr.domain.stages.is_all);
    CHECK(expr.domain.blocks.is_all);
    CHECK(expr.domain.scenarios.is_all);
  }

  TEST_CASE("Parse constraint with scenario domain")
  {
    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(scenario in {1,2}))");

    CHECK_FALSE(expr.domain.scenarios.is_all);
    REQUIRE(expr.domain.scenarios.values.size() == 2);
    CHECK(expr.domain.scenarios.values[0] == 1);
    CHECK(expr.domain.scenarios.values[1] == 2);
  }

  TEST_CASE("Parse for clause with = in for clause")
  {
    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100, for(stage = 1, block = 5))");

    REQUIRE(expr.domain.stages.values.size() == 1);
    CHECK(expr.domain.stages.values[0] == 1);
    REQUIRE(expr.domain.blocks.values.size() == 1);
    CHECK(expr.domain.blocks.values[0] == 5);
  }

  TEST_CASE("Parse for clause with all three dimensions")
  {
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
    auto expr =
        ConstraintParser::parse(R"(200 >= generator("G1").generation >= 10)");

    CHECK(expr.constraint_type == ConstraintType::RANGE);
    CHECK(expr.lower_bound.value_or(0.0) == doctest::Approx(10.0));
    CHECK(expr.upper_bound.value_or(0.0) == doctest::Approx(200.0));
  }

  // ── All element types ──────────────────────────────────────────────────

  TEST_CASE("Parse line flow constraint")
  {
    auto expr = ConstraintParser::parse(R"(line("L1_2").flow <= 300)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.element_id == "L1_2");
    CHECK(ref0.attribute == "flow");
  }

  TEST_CASE("Parse battery energy constraint")
  {
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
    auto expr = ConstraintParser::parse(R"(demand("D1").fail <= 10)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "demand");
    CHECK(ref0.attribute == "fail");
  }

  TEST_CASE("Parse demand load and fail in same expression")
  {
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
    auto expr = ConstraintParser::parse(
        R"(2 * generator("G1").generation - demand("D1").load + line("L1").flow >= 0)");

    CHECK(expr.constraint_type == ConstraintType::GREATER_EQUAL);
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
  }

  // ── Variables on both sides ────────────────────────────────────────────

  TEST_CASE("Parse with variables on both sides of >=")
  {
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
    auto expr = ConstraintParser::parse("my_limit",
                                        R"(generator("G1").generation <= 100)");

    CHECK(expr.name == "my_limit");
  }

  // ── Error paths ────────────────────────────────────────────────────────

  TEST_CASE("Error: empty expression")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse("")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: missing constraint operator")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1").generation 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: invalid element type")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(foo("G1").generation <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: unterminated string")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1).generation <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: invalid for clause dimension")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(week in {1}))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: missing attribute")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(R"(generator("G1") <= 100)")),
        std::invalid_argument);
  }

  TEST_CASE("Error: number after star is not element type")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(R"(2 * 3 <= 100)")),
        std::invalid_argument);
  }

  // ── Bare numeric UID identification ──────────────────────────────────

  TEST_CASE("Parse generator with bare numeric UID")
  {
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
    auto expr = ConstraintParser::parse(R"(flow(3).flow <= 500)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "flow");
    CHECK(ref0.element_id == "uid:3");
    CHECK(ref0.attribute == "flow");
  }

  TEST_CASE("Parse filtration element")
  {
    auto expr = ConstraintParser::parse(R"(filtration("FIL1").flow <= 200)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "filtration");
    CHECK(ref0.element_id == "FIL1");
    CHECK(ref0.attribute == "flow");
  }

  TEST_CASE("Parse reserve_provision element")
  {
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
    auto expr = ConstraintParser::parse(R"(line("L1").flowp <= 150)");

    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.attribute == "flowp");
  }

  TEST_CASE("Parse line with lossp attribute")
  {
    auto expr = ConstraintParser::parse(R"(line("L1").lossp <= 10)");

    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.attribute == "lossp");
  }

  TEST_CASE("Parse line with lossn attribute")
  {
    auto expr = ConstraintParser::parse(R"(line("L1").lossn <= 10)");

    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "line");
    CHECK(ref0.attribute == "lossn");
  }

  TEST_CASE("Parse battery with charge and discharge attributes")
  {
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
    auto expr = ConstraintParser::parse(
        "generator(\"G1\").generation <= 100 // limit gen output");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(100.0));
  }

  TEST_CASE("Parse multiline expression with comments")
  {
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
    auto expr =
        ConstraintParser::parse(R"(generator('uid:5').generation <= 200)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "uid:5");
  }

  // ── type_filter in sum() ───────────────────────────────────────────────────

  TEST_CASE("sum(all, type=) parses type_filter")
  {
    auto expr = ConstraintParser::parse(
        R"(sum(generator(all, type="hydro").generation) <= 500)");

    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    CHECK(expr.rhs == doctest::Approx(500.0));
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.element_type == "generator");
    CHECK(sr.all_elements == true);
    REQUIRE(sr.type_filter.has_value());
    CHECK(sr.type_filter.value_or("") == "hydro");
    CHECK(sr.attribute == "generation");
  }

  TEST_CASE("sum(all) without type_filter has nullopt type_filter")
  {
    auto expr =
        ConstraintParser::parse(R"(sum(generator(all).generation) <= 1000)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.all_elements == true);
    CHECK_FALSE(sr.type_filter.has_value());
  }

  TEST_CASE("sum(all, type=) with single-quote type value")
  {
    auto expr = ConstraintParser::parse(
        R"(sum(demand(all, type='industrial').load) >= 100)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].sum_ref.has_value());
    const auto& sr = expr.terms[0].sum_ref.value_or(SumElementRef {});
    CHECK(sr.element_type == "demand");
    CHECK(sr.all_elements == true);
    REQUIRE(sr.type_filter.has_value());
    CHECK(sr.type_filter.value_or("") == "industrial");
    CHECK(sr.attribute == "load");
  }

  // ── New storage attributes: spill / drain / extraction ────────────────

  TEST_CASE("Parse battery spill attribute")
  {
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
    auto expr = ConstraintParser::parse(R"(battery("B1").drain <= 5)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "battery");
    CHECK(ref0.attribute == "drain");
  }

  TEST_CASE("Parse reservoir spill attribute")
  {
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
    auto expr = ConstraintParser::parse(R"(reservoir("RES1").drain <= 50)");

    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_type == "reservoir");
    CHECK(ref0.attribute == "drain");
  }

  TEST_CASE("Parse reservoir extraction attribute")
  {
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
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1").generation @ 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: range constraint with non-constant lower bound")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= generator("G2").generation <= 100)")),
        std::invalid_argument);
  }

  TEST_CASE("Error: range constraint with non-constant upper bound")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(10 <= generator("G1").generation <= demand("D1").load)")),
        std::invalid_argument);
  }

  TEST_CASE("Error: missing 'in' or '=' after dimension in for clause")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator("G1").generation <= 100, for(stage 1))")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-element type after comma in sum(all,...)")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator(all, foo).generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-string type value in sum(all, type=)")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator(all, type=123).generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: unexpected token in sum element list")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator(<=).generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-element type in sum()")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(foo("G1").generation) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: non-ident dimension in for clause")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(123 in all))")),
        std::invalid_argument);
  }

  TEST_CASE("Lexer peek does not consume token")
  {
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
    // 'for' directly after expression (no comma)
    auto expr = ConstraintParser::parse(
        R"(generator("G1").generation <= 100 for(stage in 1..3))");

    CHECK_FALSE(expr.domain.stages.is_all);
    REQUIRE(expr.domain.stages.values.size() == 3);
  }

  TEST_CASE("Parse with escape character in string literal")
  {
    auto expr =
        ConstraintParser::parse(R"(generator("G\"1").generation <= 100)");
    REQUIRE(expr.terms.size() == 1);
    REQUIRE(expr.terms[0].element.has_value());
    const auto& ref0 = expr.terms[0].element.value_or(ElementRef {});
    CHECK(ref0.element_id == "G\"1");
  }

  TEST_CASE("Parse with leading plus sign")
  {
    auto expr =
        ConstraintParser::parse(R"(+generator("G1").generation <= 100)");
    CHECK(expr.constraint_type == ConstraintType::LESS_EQUAL);
    REQUIRE(expr.terms.size() == 1);
    CHECK(expr.terms[0].coefficient == doctest::Approx(1.0));
  }

  TEST_CASE("Error: bad element id type (not string or number)")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(generator(<=).generation <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: missing attribute after dot")
  {
    CHECK_THROWS_AS(static_cast<void>(
                        ConstraintParser::parse(R"(generator("G1"). <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: missing attribute after dot in sum")
  {
    CHECK_THROWS_AS(static_cast<void>(ConstraintParser::parse(
                        R"(sum(generator("G1"). ) <= 100)")),
                    std::invalid_argument);
  }

  TEST_CASE("Error: expected number after .. in index set")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in 1..foo))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: expected number after .. in braced index set")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in {1..foo}))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: non-number in braced index set")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in {foo}))")),
        std::invalid_argument);
  }

  TEST_CASE("Error: invalid index set starting token")
  {
    CHECK_THROWS_AS(
        static_cast<void>(ConstraintParser::parse(
            R"(generator("G1").generation <= 100, for(block in <=))")),
        std::invalid_argument);
  }
}
