// SPDX-License-Identifier: BSD-3-Clause
//
// Tests for the non-consumptive FlowRight mode (`consumptive = false`).
//
// A consumptive FlowRight subtracts its served flow (`flow_b`) from the
// `junction_a` balance only — the water leaves the system (irrigation /
// filtration offtake).  A NON-consumptive FlowRight additionally credits
// the same `flow_b` (+1) to `junction_b`, so the right keeps the
// water in the river (debit `junction_a`, credit `junction_b`, like a
// Waterway/Turbine arc) while still enforcing its [fmin, fmax] band.  This
// models a minimum river / turbined flow (cf. SDDP "minimum turbined
// outflow").

#include <doctest/doctest.h>
#include <gtopt/flow_right.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/junction.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// Unique outer namespace so the anon-namespace helpers below do not
// collide with identically-named helpers in sibling test files when CMake
// batches them into a single unity-build translation unit.
namespace frnc_test
{

namespace
{

[[nodiscard]] Simulation make_single_block_simulation()
{
  return {
      .block_array = {{.uid = Uid {1}, .duration = 1.0}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
}

[[nodiscard]] PlanningOptions make_unscaled_options()
{
  PlanningOptions popts;
  popts.model_options.scale_objective = 1.0;
  popts.model_options.demand_fail_cost = 1000.0;
  return popts;
}

/// j_src (1) and j_ret (2) each anchored by a waterway to a drain (3), so
/// both carry balance rows the FlowRight can couple into.
[[nodiscard]] System make_system(const Array<FlowRight>& flow_rights)
{
  return System {
      .name = "FlowRightNonConsumptiveFixture",
      .bus_array = {{.uid = Uid {1}, .name = "b1"}},
      .junction_array =
          {
              {.uid = Uid {1}, .name = "j_src"},
              {.uid = Uid {2}, .name = "j_ret"},
              {.uid = Uid {3}, .name = "j_drain", .drain = true},
          },
      .waterway_array =
          {
              {.uid = Uid {1},
               .name = "ww_src",
               .junction_a = Uid {1},
               .junction_b = Uid {3},
               .fmin = 0.0,
               .fmax = 1000.0},
              {.uid = Uid {2},
               .name = "ww_ret",
               .junction_a = Uid {2},
               .junction_b = Uid {3},
               .fmin = 0.0,
               .fmax = 1000.0},
          },
      .flow_right_array = flow_rights,
  };
}

[[nodiscard]] const JunctionLP* find_junction(const SystemLP& system_lp,
                                              Uid uid)
{
  for (const auto& j : system_lp.elements<JunctionLP>()) {
    if (j.uid() == uid) {
      return &j;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE(
    "FlowRight consumptive vs non-consumptive junction coupling")  // NOLINT
{
  const auto sim = make_single_block_simulation();
  const PlanningOptionsLP options(make_unscaled_options());

  SUBCASE("non-consumptive credits the served flow to junction_b")
  {
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_nc",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = 5.0,
            .fmax = 10.0,
            .junction_b = Uid {2},
            .consumptive = OptBool {false},
        },
    };
    const auto sys = make_system(frs);
    SimulationLP simulation_lp(sim, options);
    SystemLP system_lp(sys, simulation_lp);

    auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];

    const auto& fcols = fr_lp.flow_cols_at(s, t);
    REQUIRE(fcols.size() == 1);
    const auto fcol = fcols.begin()->second;
    const auto buid = fcols.begin()->first;

    const auto* j_src = find_junction(system_lp, Uid {1});
    const auto* j_ret = find_junction(system_lp, Uid {2});
    REQUIRE(j_src != nullptr);
    REQUIRE(j_ret != nullptr);

    const auto src_row = j_src->balance_rows_at(s, t).at(buid);
    const auto ret_row = j_ret->balance_rows_at(s, t).at(buid);

    // Served flow leaves j_src (−1) AND arrives at j_ret (+1): conserved.
    CHECK(lp.get_coeff(src_row, fcol) == doctest::Approx(-1.0));
    CHECK(lp.get_coeff(ret_row, fcol) == doctest::Approx(+1.0));
  }

  SUBCASE("default (consumptive) does not credit junction_b")
  {
    // junction_b still set (its separate bypass column exists), but
    // the SERVED flow_b is consumptive — no +1 on the served column at the
    // return junction.
    const Array<FlowRight> frs = {
        {
            .uid = Uid {1},
            .name = "fr_c",
            .junction_a = Uid {1},
            .direction = -1,
            .fmin = 5.0,
            .fmax = 10.0,
            .junction_b = Uid {2},
        },
    };
    const auto sys = make_system(frs);
    SimulationLP simulation_lp(sim, options);
    SystemLP system_lp(sys, simulation_lp);

    auto& lp = system_lp.linear_interface();
    const auto& fr_lp = system_lp.elements<FlowRightLP>().front();
    const auto& s = system_lp.scene().scenarios()[0];
    const auto& t = system_lp.phase().stages()[0];

    const auto& fcols = fr_lp.flow_cols_at(s, t);
    REQUIRE(fcols.size() == 1);
    const auto fcol = fcols.begin()->second;
    const auto buid = fcols.begin()->first;

    const auto* j_src = find_junction(system_lp, Uid {1});
    const auto* j_ret = find_junction(system_lp, Uid {2});
    REQUIRE(j_src != nullptr);
    REQUIRE(j_ret != nullptr);

    const auto src_row = j_src->balance_rows_at(s, t).at(buid);
    const auto ret_row = j_ret->balance_rows_at(s, t).at(buid);

    CHECK(lp.get_coeff(src_row, fcol) == doctest::Approx(-1.0));
    // The served flow column is NOT credited at the return junction.
    CHECK(lp.get_coeff(ret_row, fcol) == doctest::Approx(0.0));
  }
}

}  // namespace frnc_test
