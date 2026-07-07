/**
 * @file      test_ampl_new_attributes.cpp
 * @brief     User-constraint coverage for newly-exposed AMPL attributes
 * @date      2026-05-19
 * @copyright BSD-3-Clause
 *
 * The AMPL dispatch refactor exposed several `*LP::param_*` accessors
 * that the legacy if/else chain in `element_column_resolver.cpp` never
 * routed.  These tests pin the new resolver path end-to-end: each user
 * constraint references one of the newly-exposed attributes by name,
 * and `constraint_mode: strict` is used so that an unresolved attribute
 * propagates as an exception rather than a silent skip.  If a future
 * regression drops the entry from `register_ampl_param_dispatchers`,
 * the corresponding test fails immediately.
 *
 * Covered attributes:
 *   - `line.voltage`            (per-stage, pu)
 *   - `line.tap_ratio`          (per-stage, transformer)
 *   - `line.phase_shift_deg`    (per-stage, phase shifter)
 *   - `line.lossfactor`         (per-(stage, block))
 *   - `line.resistance`         (per-stage)
 *   - `emission_zone.cap`       (per-stage tCO₂ cap)
 *   - `emission_zone.cap_cost`  (per-stage $/t soft cap)
 *   - `emission_zone.price`     (per-stage $/t carbon price)
 *   - `emission_source.rate`           (per-stage tCO₂/MWh)
 *   - `emission_source.upstream_rate`  (per-stage tCO₂/MWh)
 */

#include <format>
#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

using namespace gtopt;

namespace
{

/// Build a single-bus / single-stage / single-block Planning JSON with
/// a `Line` carrying the supplied per-stage attribute value and a user
/// constraint that references `line('l1').<attr>` as an additive term:
///
///   generator('g1').generation + 0 * line('l1').<attr> <= 100
///
/// Zero coefficient means the constraint is equivalent to
/// `generation <= 100` for solve purposes — the test asserts only that
/// the resolver wires the attribute up; the LP-side semantics are the
/// per-attribute accessor's responsibility.  Using a non-zero
/// coefficient would force every test to know the schedule's physical
/// interpretation, which is out of scope.
[[nodiscard]] std::string line_attr_planning(std::string_view attr,
                                             std::string_view extra_line_field)
{
  return std::format(R"json(
{{
  "options": {{
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "constraint_mode": "strict",
    "model_options": {{
      "use_single_bus": true,
      "scale_objective": 1,
      "demand_fail_cost": 1000
    }}
  }},
  "simulation": {{
    "block_array": [{{ "uid": 1, "duration": 1 }}],
    "stage_array": [{{ "uid": 1, "first_block": 0, "count_block": 1 }}],
    "scenario_array": [{{ "uid": 1 }}]
  }},
  "system": {{
    "name": "ampl_line_attr_{0}",
    "bus_array": [
      {{ "uid": 1, "name": "b1" }},
      {{ "uid": 2, "name": "b2" }}
    ],
    "line_array": [
      {{ "uid": 1, "name": "l1", "bus_a": 1, "bus_b": 2,
         "tmax_ab": 1000, "tmax_ba": 1000, "reactance": 0.1, {1} }}
    ],
    "generator_array": [
      {{ "uid": 1, "name": "g1", "bus": "b1",
         "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200 }}
    ],
    "demand_array": [
      {{ "uid": 1, "name": "d1", "bus": "b2",
         "lmax": [[ 50.0 ]] }}
    ],
    "user_constraint_array": [
      {{
        "uid": 1,
        "name": "uc_line_{0}",
        "expression": "generator('g1').generation + 0 * line('l1').{0} <= 100"
      }}
    ]
  }}
}}
)json",
                     attr,
                     extra_line_field);
}

}  // namespace

TEST_CASE("AMPL new attribute - line.voltage resolves in user constraint")
{
  auto planning =
      parse_planning_json(line_attr_planning("voltage", R"("voltage": 1.0)"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE("AMPL new attribute - line.tap_ratio resolves in user constraint")
{
  auto planning = parse_planning_json(
      line_attr_planning("tap_ratio", R"("tap_ratio": 1.05)"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE(
    "AMPL new attribute - line.phase_shift_deg resolves in user constraint")
{
  auto planning = parse_planning_json(
      line_attr_planning("phase_shift_deg", R"("phase_shift_deg": 3.0)"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE("AMPL new attribute - line.lossfactor resolves in user constraint")
{
  auto planning = parse_planning_json(
      line_attr_planning("lossfactor", R"("lossfactor": 0.02)"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE("AMPL new attribute - line.resistance resolves in user constraint")
{
  auto planning = parse_planning_json(
      line_attr_planning("resistance", R"("resistance": 0.05)"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

// ── Emission attributes ─────────────────────────────────────────────────────

namespace
{

/// Planning JSON with a single emission zone + emission source.  The
/// user constraint references one of the two via `<attr>` so that the
/// resolver path is exercised under `constraint_mode: strict`.
[[nodiscard]] std::string emission_attr_planning(std::string_view element_type,
                                                 std::string_view element_name,
                                                 std::string_view attr)
{
  return std::format(R"json(
{{
  "options": {{
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "constraint_mode": "strict",
    "model_options": {{
      "use_single_bus": true,
      "scale_objective": 1,
      "demand_fail_cost": 1000
    }}
  }},
  "simulation": {{
    "block_array": [{{ "uid": 1, "duration": 1 }}],
    "stage_array": [{{ "uid": 1, "first_block": 0, "count_block": 1 }}],
    "scenario_array": [{{ "uid": 1 }}]
  }},
  "system": {{
    "name": "ampl_emission_{0}_{2}",
    "bus_array": [{{ "uid": 1, "name": "b1" }}],
    "generator_array": [
      {{ "uid": 1, "name": "g1", "bus": "b1",
         "pmin": 0, "pmax": 100, "gcost": 20, "capacity": 100 }}
    ],
    "demand_array": [
      {{ "uid": 1, "name": "d1", "bus": "b1", "lmax": [[ 50.0 ]] }}
    ],
    "emission_array": [
      {{ "uid": 1, "name": "co2" }}
    ],
    "emission_zone_array": [
      {{ "uid": 1, "name": "ez1", "emission": 1,
         "cap": 1000.0, "cap_cost": 50.0, "price": 25.0 }}
    ],
    "emission_source_array": [
      {{ "uid": 1, "name": "es1",
         "zone": 1, "generator": 1, "emission": 1,
         "rate": 0.4, "upstream_rate": 0.05 }}
    ],
    "user_constraint_array": [
      {{
        "uid": 1,
        "name": "uc_{0}_{2}",
        "expression": "generator('g1').generation + 0 * {0}('{1}').{2} <= 100"
      }}
    ]
  }}
}}
)json",
                     element_type,
                     element_name,
                     attr);
}

}  // namespace

TEST_CASE("AMPL new attribute - emission_zone.cap resolves in user constraint")
{
  auto planning = parse_planning_json(
      emission_attr_planning("emission_zone", "ez1", "cap"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE(
    "AMPL new attribute - emission_zone.cap_cost resolves in user constraint")
{
  auto planning = parse_planning_json(
      emission_attr_planning("emission_zone", "ez1", "cap_cost"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE(
    "AMPL new attribute - emission_zone.price resolves in user constraint")
{
  auto planning = parse_planning_json(
      emission_attr_planning("emission_zone", "ez1", "price"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE(
    "AMPL new attribute - emission_source.rate resolves in user constraint")
{
  auto planning = parse_planning_json(
      emission_attr_planning("emission_source", "es1", "rate"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}

TEST_CASE(
    "AMPL new attribute - emission_source.upstream_rate resolves in user "
    "constraint")
{
  auto planning = parse_planning_json(
      emission_attr_planning("emission_source", "es1", "upstream_rate"));
  PlanningLP planning_lp(std::move(planning));
  REQUIRE(planning_lp.resolve().has_value());
}
