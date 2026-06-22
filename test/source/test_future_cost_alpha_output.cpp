// SPDX-License-Identifier: BSD-3-Clause
//
// Piece-2 C/D: a FutureCost element saves the cost-to-go α (and, under
// multicut, every per-source-scene varphi_s) to the SDDP solution.  The α / cut
// / rebase machinery is untouched — the solve method only copies the α column
// handle(s) + the per-scene rebase constant c̄ onto each cell's FutureCostLP,
// which `add_to_output` then emits.  This is read-only w.r.t. the LP, so the
// SDDP bounds are unchanged (see test_sddp_boundary_cuts_mean_shift).

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/future_cost.hpp>
#include <gtopt/future_cost_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/utils.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("FutureCost element saves alpha to the SDDP solution")  // NOLINT
{
  // One boundary cut so the terminal α is bound (not pinned at 0).
  const auto cuts_file =
      (std::filesystem::temp_directory_path() / "gtopt_test_fc_alpha_out.csv")
          .string();
  {
    std::ofstream ofs(cuts_file);
    ofs << "iteration,scene,rhs,rsv1\n";
    ofs << "1,1,1000.0,2.0\n";
  }

  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  // A FutureCost element makes each (scene, phase) cell carry a FutureCostLP
  // that saves its α — without it, the α is built but never written.
  planning.system.future_cost_array.push_back(
      FutureCost {.uid = Uid {1}, .name = "fcf"});
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.convergence_tol = 1e-6;
  sddp_opts.enable_api = false;
  sddp_opts.boundary_cuts_file = cuts_file;
  sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;
  sddp_opts.low_memory_mode = LowMemoryMode::off;  // keep FutureCostLP resident

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  // After solve, the α machinery populated each cell's FutureCostLP with its
  // α output stream(s) — the single-α layout here gives one "alpha" stream per
  // cell that registered an α column.
  std::size_t cells_with_fc = 0;
  std::size_t total_streams = 0;
  const auto& sim = planning_lp.simulation();
  const auto n_phases = static_cast<Index>(sim.phases().size());
  for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
    for (const auto pi : iota_range<PhaseIndex>(0, n_phases)) {
      auto& sys = planning_lp.system(si, pi);
      for (const auto& fc : sys.elements<FutureCostLP>()) {
        ++cells_with_fc;
        total_streams += fc.alpha_stream_count();
      }
    }
  }
  CHECK(cells_with_fc > 0);  // the FutureCost element instantiated per cell
  CHECK(total_streams > 0);  // α was saved on at least one cell

  std::filesystem::remove(cuts_file);
}
