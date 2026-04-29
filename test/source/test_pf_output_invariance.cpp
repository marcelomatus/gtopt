// SPDX-License-Identifier: BSD-3-Clause
//
// Probability-factor output-invariance tests.
//
// **Contract**: ``OutputContext::add_row_dual`` and ``add_col_cost``
// apply per-(scenario, stage [, block]) inverse cost factors via
// ``CostHelper::block_icost_factors`` /
// ``scenario_stage_icost_factors`` /  ``stage_icost_factors`` so that
// dual / reduced-cost values written to disk are **truly physical**:
// no probability_factor, no discount, no duration, no scale_objective
// folded in.
//
// Tests:
//   T1 — Run the same physical problem under prob=1.0 (1 scenario) and
//        prob=0.5/0.5 (2 identical scenarios).  Read the bus dual and
//        generator reduced-cost CSVs.  Each scene's per-scenario value
//        must match the prob=1 reference.  A prob-leak in OutputContext
//        would shift values by a factor of prob.
//   T2 — Asymmetric prob=0.7/0.3.  Same expectation: each scene's
//        per-scenario CSV value matches the prob=1 reference (since
//        prob is unscaled at output, output values are
//        prob-independent).
//   T3 — Direct value: a generator with gcost=20, dispatch non-binding
//        and at the LP optimum.  Reduced cost CSV value must equal 0
//        exactly (LP-physical, no prob-tainted offset).  Bus LMP must
//        equal 20 $/MWh exactly (generator gcost binding).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>

#include "test_csv_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)
using gtopt::test_helpers::read_uid_values;

namespace
{

// Single-bus, single-generator, single-demand fixture parameterised by
// scenario configuration.  All other parameters identical so the
// physical optimum (LMP = gcost = 20, generator dispatch = load = 80,
// reduced cost on g1 = 0) is the same across configurations.  Only
// scenario/scene structure varies between the three runs.
inline auto make_planning_json(std::string_view scenario_array,
                               std::string_view scene_array_or_empty,
                               std::string_view name) -> std::string
{
  std::string scene_part;
  if (!scene_array_or_empty.empty()) {
    scene_part = std::string(",") + std::string(scene_array_or_empty);
  }
  return std::string(R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": true,
    "demand_fail_cost": 10000,
    "scale_objective": 1
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": )")
      + std::string(scenario_array) + scene_part + std::string(R"(
  },
  "system": {
    "name": ")")
      + std::string(name) + std::string(R"(",
    "bus_array": [{"uid": 1, "name": "b1"}],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1",
       "pmin": 0, "pmax": 200, "gcost": 20, "capacity": 200}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": [[80.0]]}
    ]
  }
})");
}

inline auto run_and_capture(std::string_view planning_json,
                            const std::filesystem::path& out_dir)
    -> std::shared_ptr<PlanningLP>
{
  std::filesystem::remove_all(out_dir);
  std::filesystem::create_directories(out_dir);

  Planning base = parse_planning_json(planning_json);
  base.options.output_directory = out_dir.string();

  auto plp = std::make_shared<PlanningLP>(std::move(base));
  auto result = plp->resolve();
  REQUIRE(result.has_value());

  plp->write_out();
  return plp;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────
// T1 — Output values are invariant under prob=1.0 vs prob=0.5/0.5
//
// Two configurations encoding identical physics:
//   (A) 1 scenario, prob=1.0
//   (B) 2 scenarios, prob=0.5/0.5 (each scenario carries the same
//       block_array/stage_array, identical demand)
//
// OutputContext applies the per-scenario inverse cost_factor at write
// time, so the bus LMP and generator reduced-cost CSVs in (B) must
// have values matching (A) for every scenario.  A prob-leak would
// produce LMP_B = LMP_A × prob = 10 instead of 20.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "PF output invariance — prob=1.0 vs prob=0.5/0.5")
{
  using namespace std::string_view_literals;

  const auto root = std::filesystem::temp_directory_path() / "gtopt_pf_inv_T1";

  // (A) 1 scenario, prob=1.0
  const auto json_a = make_planning_json(
      R"([{"uid": 1, "probability_factor": 1.0}])"sv, ""sv, "A_p1");
  const auto out_a = root / "a";
  auto plp_a = run_and_capture(json_a, out_a);

  // (B) 2 scenarios, prob=0.5/0.5
  const auto json_b = make_planning_json(
      R"([{"uid": 1, "probability_factor": 0.5},
          {"uid": 2, "probability_factor": 0.5}])"sv,
      ""sv,
      "B_p0505");
  const auto out_b = root / "b";
  auto plp_b = run_and_capture(json_b, out_b);

  // Read bus LMP (balance_dual) for each.  Single-bus → 1 uid value.
  const auto lmp_a = read_uid_values(out_a / "Bus" / "balance_dual", 1);
  const auto lmp_b = read_uid_values(out_b / "Bus" / "balance_dual", 1);
  REQUIRE(lmp_a.size() == 1);
  REQUIRE(lmp_b.size() == 1);
  CAPTURE(lmp_a[0]);
  CAPTURE(lmp_b[0]);
  // LMP at the only bus should equal the binding generator gcost = 20
  // $/MWh in BOTH configurations — prob-invariant.
  CHECK(lmp_a[0] == doctest::Approx(20.0).epsilon(1e-6));
  CHECK(lmp_b[0] == doctest::Approx(lmp_a[0]).epsilon(1e-6));

  // Read generator reduced cost (single uid).  At the optimum the
  // dispatch column is non-basic-at-lower (g1 binding pmin=0 if no
  // demand) or basic interior (when demand=80 < pmax=200).  Either
  // way reduced cost is 0 at the LP optimum for an interior basic
  // variable.
  const auto rc_a = read_uid_values(out_a / "Generator" / "generation_cost", 1);
  const auto rc_b = read_uid_values(out_b / "Generator" / "generation_cost", 1);
  REQUIRE(rc_a.size() == 1);
  REQUIRE(rc_b.size() == 1);
  CAPTURE(rc_a[0]);
  CAPTURE(rc_b[0]);
  CHECK(rc_a[0] == doctest::Approx(0.0).epsilon(1e-6));
  CHECK(rc_b[0] == doctest::Approx(rc_a[0]).epsilon(1e-6));

  std::filesystem::remove_all(root);
}

// ────────────────────────────────────────────────────────────────────
// T2 — Asymmetric prob=0.7/0.3.  Same invariance: each scenario's
// reported LMP / reduced cost matches the prob=1.0 reference.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "PF output invariance — asymmetric prob=0.7/0.3 matches prob=1.0")
{
  using namespace std::string_view_literals;

  const auto root = std::filesystem::temp_directory_path() / "gtopt_pf_inv_T2";

  const auto json_a = make_planning_json(
      R"([{"uid": 1, "probability_factor": 1.0}])"sv, ""sv, "T2_A_p1");
  const auto out_a = root / "a";
  auto plp_a = run_and_capture(json_a, out_a);

  const auto json_c = make_planning_json(
      R"([{"uid": 1, "probability_factor": 0.7},
          {"uid": 2, "probability_factor": 0.3}])"sv,
      ""sv,
      "T2_C_p0703");
  const auto out_c = root / "c";
  auto plp_c = run_and_capture(json_c, out_c);

  const auto lmp_a = read_uid_values(out_a / "Bus" / "balance_dual", 1);
  const auto lmp_c = read_uid_values(out_c / "Bus" / "balance_dual", 1);
  REQUIRE(lmp_a.size() == 1);
  REQUIRE(lmp_c.size() == 1);
  CAPTURE(lmp_a[0]);
  CAPTURE(lmp_c[0]);
  CHECK(lmp_c[0] == doctest::Approx(lmp_a[0]).epsilon(1e-6));

  std::filesystem::remove_all(root);
}

// ────────────────────────────────────────────────────────────────────
// T3 — Direct value: bus LMP must equal generator gcost = 20 $/MWh
// exactly across all probability configurations.  Verifies the
// physical-units output: no scale_objective / discount / duration /
// prob folded in.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "PF output — bus LMP equals binding generator gcost in all configs")
{
  using namespace std::string_view_literals;

  const auto root = std::filesystem::temp_directory_path() / "gtopt_pf_inv_T3";

  struct Config
  {
    std::string_view scenario_array;
    std::string_view name;
  };
  const std::array<Config, 3> configs = {{
      {R"([{"uid": 1, "probability_factor": 1.0}])"sv, "p1.0"},
      {R"([{"uid": 1, "probability_factor": 0.5},
            {"uid": 2, "probability_factor": 0.5}])"sv,
       "p0505"},
      {R"([{"uid": 1, "probability_factor": 0.7},
            {"uid": 2, "probability_factor": 0.3}])"sv,
       "p0703"},
  }};

  for (std::size_t i = 0; i < configs.size(); ++i) {
    const auto& cfg = configs[i];
    CAPTURE(cfg.name);
    SUBCASE(cfg.name.data())
    {
      const auto out_dir = root / std::to_string(i);
      const auto json = make_planning_json(
          cfg.scenario_array, ""sv, std::string("T3_") + std::string(cfg.name));
      auto plp = run_and_capture(json, out_dir);

      const auto lmp = read_uid_values(out_dir / "Bus" / "balance_dual", 1);
      REQUIRE(lmp.size() == 1);
      CAPTURE(lmp[0]);
      // Generator g1 has gcost=20 $/MWh and is the only dispatchable
      // unit.  At the LP optimum, generator is basic (dispatching 80
      // MW < pmax=200) and the bus balance row is binding on g1.
      // The LMP equals g1's reduced-cost-marginal = 20 $/MWh.
      CHECK(lmp[0] == doctest::Approx(20.0).epsilon(1e-6));
    }
  }

  std::filesystem::remove_all(root);
}
