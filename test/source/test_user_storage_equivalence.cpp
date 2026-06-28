// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_user_storage_equivalence.cpp
 * @brief     A reservoir modeled purely in the AMPL/user-constraint text layer
 *            reproduces the native gtopt reservoir under SDDP.
 * @date      2026-06-28
 * @copyright BSD-3-Clause
 *
 * Validates the `block_state` DecisionVariable + `prev(...)` lag feature: a
 * hydro reservoir written as a per-block `DecisionVariable` (the volume) plus
 * `UserConstraint` rows (a within-phase / cross-phase balance + terminal
 * target) must give the SAME SDDP optimum as the native `Reservoir`
 * (`StorageLP`) it replaces.
 *
 * Oracle: `make_2scene_10phase_two_reservoir_planning()` (2 scenes, 10 phases,
 * 2 native reservoirs, hard efin=150, fcr=1, production_factor=1, 1 block /
 * phase).  For reservoir 1 the native chain
 *   inflow → junction → waterway(turbine) → generator, plus the storage
 *   balance  vol_b = vol_{b-1} − fcr·dur·extraction  and the junction balance
 *   extraction = waterway_flow − inflow = generation − inflow
 * collapses (fcr=dur=pf=1) to
 *   vol_b = vol_{b-1} + inflow − generation.
 *
 * The candidate replaces reservoir 1's whole sub-topology {Reservoir,
 * Junction, Waterway, Turbine, Flow} with a `block_state` DecisionVariable
 * `vol1` (the volume) and three user constraints expressing exactly that
 * balance + the efin terminal target, keeping the hydro generator as the
 * release variable.  Reservoir 2 stays native — so one solve exercises mixed
 * native + AMPL cross-phase state coupling.
 */

#include <algorithm>
#include <cmath>
#include <utility>

#include <doctest/doctest.h>
#include <gtopt/decision_variable.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace user_storage_equiv_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

[[nodiscard]] auto solve_sddp_bounds(Planning planning)
    -> std::pair<double, double>
{
  planning.options.method = MethodType::sddp;
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 8;
  opts.convergence_tol = 1.0e-4;
  opts.cut_sharing = CutSharingMode::none;
  opts.enable_api = false;

  SDDPMethod sddp(planning_lp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  REQUIRE_FALSE(results->empty());
  return {results->back().lower_bound, results->back().upper_bound};
}

/// Clone the native 2-reservoir fixture and replace reservoir 1's hydro
/// sub-topology with an AMPL-text reservoir (a `block_state` DecisionVariable
/// + balance/terminal user constraints).  Reservoir 2 stays native.
[[nodiscard]] auto make_ampl_reservoir_candidate() -> Planning
{
  auto p = make_2scene_10phase_two_reservoir_planning();
  auto& sys = p.system;

  // Remove reservoir 1's entire native sub-topology (all uid == 1).  The
  // hydro generator (hydro_gen_1, uid 1) stays — it is now the release.
  std::erase_if(sys.reservoir_array,
                [](const Reservoir& r) { return r.uid == Uid {1}; });
  std::erase_if(sys.junction_array,
                [](const Junction& j) { return j.uid == Uid {1}; });
  std::erase_if(sys.waterway_array,
                [](const Waterway& w) { return w.uid == Uid {1}; });
  std::erase_if(sys.turbine_array,
                [](const Turbine& t) { return t.uid == Uid {1}; });
  std::erase_if(sys.flow_array, [](const Flow& f) { return f.uid == Uid {1}; });

  // The AMPL reservoir volume: a per-block storage state, eini=0, [0, 200].
  sys.decision_variable_array.push_back(DecisionVariable {
      .uid = Uid {101},
      .name = "vol1",
      .lower_bound = OptReal {0.0},
      .upper_bound = OptReal {200.0},
      .scope = OptName {"block"},
      .state = OptBool {},
      .link = OptBool {true},
      .block_state = OptBool {true},
      .initial_value = OptReal {0.0},
  });

  // Balance: vol1 - prev(vol1) + generation = inflow.  Inflow is 80 in phase
  // 0 (stage uid 1) and 20 in phases 1..9 (stage uids 2..10), so it is a
  // per-stage RHS constant expressed with two for(...)-scoped rows.
  sys.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {201},
      .name = "vol1_balance_first",
      .expression = "decision_variable('vol1').value "
                    "- prev(decision_variable('vol1').value) "
                    "+ generator('hydro_gen_1').generation = 80, "
                    "for(stage in {1})",
  });
  sys.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {202},
      .name = "vol1_balance_rest",
      .expression = "decision_variable('vol1').value "
                    "- prev(decision_variable('vol1').value) "
                    "+ generator('hydro_gen_1').generation = 20, "
                    "for(stage in {2,3,4,5,6,7,8,9,10})",
  });
  // Terminal target (efin): vol1 >= 150 at the last stage.
  sys.user_constraint_array.push_back(UserConstraint {
      .uid = Uid {203},
      .name = "vol1_efin",
      .expression =
          "decision_variable('vol1').value >= 150, for(stage in {10})",
  });

  return p;
}

}  // namespace
}  // namespace user_storage_equiv_test

TEST_CASE(
    "AMPL-text reservoir reproduces the native reservoir SDDP bounds "
    "(2-reservoir 10-phase, one reservoir replaced)")
{
  using namespace user_storage_equiv_test;  // NOLINT(google-build-using-namespace)

  const auto [lb_native, ub_native] =
      solve_sddp_bounds(make_2scene_10phase_two_reservoir_planning());
  const auto [lb_ampl, ub_ampl] =
      solve_sddp_bounds(make_ampl_reservoir_candidate());

  CHECK(std::isfinite(lb_native));
  CHECK(std::isfinite(ub_native));
  CHECK(std::isfinite(lb_ampl));
  CHECK(std::isfinite(ub_ampl));

  // The AMPL reservoir is algebraically identical to the native one, so the
  // SDDP bounds must agree (deterministic fixed hydrology).
  CHECK(lb_ampl == doctest::Approx(lb_native).epsilon(1e-3));
  CHECK(ub_ampl == doctest::Approx(ub_native).epsilon(1e-3));
}

TEST_CASE("AMPL-text reservoir registers a per-phase cross-phase state")
{
  using namespace user_storage_equiv_test;  // NOLINT(google-build-using-namespace)

  // Structural: the block_state DecisionVariable must register a
  // StateVariable in every phase under the dedicated BlockStateClassName,
  // linking phases 0..8 forward and terminal at phase 9 — exactly like the
  // native reservoir efin.
  PlanningLP planning_lp(make_ampl_reservoir_candidate());
  const auto& sim = planning_lp.simulation();
  REQUIRE(static_cast<int>(sim.phases().size()) == 10);
  const auto scene = first_scene_index();

  for (int pi = 0; pi < 10; ++pi) {
    const auto& sv_map = sim.state_variables(scene, PhaseIndex {pi});
    bool found = false;
    for (const auto& [key, svar] : sv_map) {
      if (key.class_name == DecisionVariableLP::BlockStateClassName
          && key.uid == Uid {101})
      {
        found = true;
        if (pi < 9) {
          CHECK_FALSE(svar.dependent_variables().empty());
        } else {
          CHECK(svar.dependent_variables().empty());  // last phase
        }
      }
    }
    CHECK(found);
  }
}

// ── Guards: block_state misuse is a hard error ─────────────────────────────

namespace user_storage_equiv_test  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
[[nodiscard]] auto with_dv(DecisionVariable dv) -> Planning
{
  auto p = make_2scene_10phase_two_reservoir_planning();
  p.system.decision_variable_array.push_back(std::move(dv));
  return p;
}
}  // namespace
}  // namespace user_storage_equiv_test

TEST_CASE(
    "block_state guards: link required, block scope, exclusive with state")
{
  using namespace user_storage_equiv_test;  // NOLINT(google-build-using-namespace)

  SUBCASE("block_state without link is a hard error")
  {
    CHECK_THROWS_AS(  // NOLINT
        PlanningLP(with_dv(DecisionVariable {
            .uid = Uid {900}, .name = "bad", .block_state = OptBool {true}})),
        std::runtime_error);
  }
  SUBCASE("block_state with a coarse scope is a hard error")
  {
    CHECK_THROWS_AS(  // NOLINT
        PlanningLP(with_dv(DecisionVariable {.uid = Uid {901},
                                             .name = "bad",
                                             .scope = OptName {"phase"},
                                             .link = OptBool {true},
                                             .block_state = OptBool {true}})),
        std::runtime_error);
  }
  SUBCASE("block_state together with state is a hard error")
  {
    CHECK_THROWS_AS(  // NOLINT
        PlanningLP(with_dv(DecisionVariable {.uid = Uid {902},
                                             .name = "bad",
                                             .scope = OptName {"global"},
                                             .state = OptBool {true},
                                             .link = OptBool {true},
                                             .block_state = OptBool {true}})),
        std::runtime_error);
  }
}
