// SPDX-License-Identifier: BSD-3-Clause
//
// Piece-2 C/D: a FutureCost element saves the cost-to-go α (and, under
// multicut, every per-source-scene varphi_s) to the SDDP solution.  The α / cut
// / rebase machinery is untouched — `FutureCostLP::add_to_output` SELF-FINDS
// its data at write time from the persistent `SimulationLP` registries (the
// state-variable map for the α columns, `alpha_offset` for the rebase c̄).
// Those registries survive the per-cell LP rebuild that `write_out` performs
// under `low_memory = compress`, so the α-output works under ALL low_memory
// modes (the test exercises both `off` and `compress`).  This is read-only
// w.r.t. the LP, so the SDDP bounds are unchanged (see
// test_sddp_boundary_cuts_mean_shift).

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/future_cost.hpp>
#include <gtopt/future_cost_lp.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("FutureCost element saves alpha to the SDDP solution")  // NOLINT
{
  // The self-find data — α columns on the terminal cells + the per-scene
  // rebase offset — must be available after solve() under EVERY low_memory
  // mode, because `FutureCostLP::add_to_output` reads it from the persistent
  // SimulationLP at write time rather than from a per-cell stash.
  const auto run_mode = [](LowMemoryMode mode)
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
    // Real write_out below: route the element streams to a scratch dir so
    // the FutureCostLP::add_to_output emission itself is exercised (the
    // registry checks alone never call it).
    const auto out_dir = std::filesystem::temp_directory_path()
        / ("gtopt_test_fc_alpha_outdir_"
           + std::to_string(static_cast<int>(mode)));
    std::filesystem::remove_all(out_dir);
    planning.options.output_directory = out_dir.string();
    planning.options.output_format = DataFormat::csv;
    // A FutureCost element makes each (scene, phase) cell carry a FutureCostLP
    // that emits its α at write_out — without it nothing routes the stream.
    planning.system.future_cost_array.push_back(
        FutureCost {.uid = Uid {1}, .name = "fcf"});
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 2;
    sddp_opts.convergence_tol = 1e-6;
    sddp_opts.enable_api = false;
    sddp_opts.boundary_cuts_file = cuts_file;
    sddp_opts.boundary_cuts_mode = BoundaryCutsMode::combined;
    sddp_opts.low_memory_mode = mode;

    SDDPMethod sddp(planning_lp, sddp_opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());

    // After solve, the self-find data is reachable straight off the persistent
    // SimulationLP: for at least one terminal cell, the α column(s) are
    // registered (so add_to_output can emit "alpha"/"alpha_<s>") and the
    // per-scene rebase offset reflects the loaded boundary cut.
    const auto& sim = planning_lp.simulation();
    const auto n_phases = static_cast<Index>(sim.phases().size());
    std::size_t cells_with_alpha = 0;
    bool any_offset_set = false;
    for (const auto si : iota_range<SceneIndex>(0, sim.scene_count())) {
      // The rebase offset is published per scene (0 when no shift landed).
      if (sim.alpha_offset(si) != 0.0) {
        any_offset_set = true;
      }
      for (const auto pi : iota_range<PhaseIndex>(0, n_phases)) {
        const auto acols = alpha_cols_on_cell(sim, si, pi);
        if (!acols.empty()) {
          ++cells_with_alpha;
        }
      }
      // FutureCostLP is a disposable collection: under compress it is
      // dropped after solve and `write_out()` re-materialises it on demand
      // via `rebuild_collections_if_needed()` (system_lp.cpp).  `update_lp`
      // deliberately does NOT rehydrate it (see test_system_lp_lazy_rebuild),
      // so mirror write_out's rebuild here before asserting the collection
      // is present for the emit.
      for (const auto pi : iota_range<PhaseIndex>(0, n_phases)) {
        auto& sys = planning_lp.system(si, pi);
        sys.rebuild_collections_if_needed();
        CHECK(!sys.elements<FutureCostLP>().empty());
      }
    }
    // α available for self-find on at least one cell (non-last phases register
    // α; the boundary cut binds the terminal one).
    CHECK(cells_with_alpha > 0);
    // The mean-shift rebase fired on the boundary-cut scene → non-zero offset
    // surfaced through the persistent SimulationLP.
    CHECK(any_offset_set);

    // Drive the actual emission: FutureCostLP::add_to_output runs per cell
    // inside write_out and must produce FutureCost/alpha* streams (plus a
    // rebase* stream on the offset-bearing scene).
    planning_lp.write_out();
    const auto fc_dir = out_dir / "FutureCost";
    REQUIRE(std::filesystem::is_directory(fc_dir));
    std::size_t alpha_files = 0;
    std::size_t rebase_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fc_dir)) {
      const auto stem = entry.path().filename().string();
      if (stem.starts_with("alpha")) {
        ++alpha_files;
        // Each alpha CSV must carry at least a header and one data row.
        std::ifstream ifs(entry.path());
        std::string header;
        std::string row;
        CHECK(static_cast<bool>(std::getline(ifs, header)));
        CHECK(static_cast<bool>(std::getline(ifs, row)));
      }
      if (stem.starts_with("rebase")) {
        ++rebase_files;
      }
    }
    CHECK(alpha_files > 0);
    // any_offset_set was asserted above, so the c_bar != 0 branch must have
    // emitted at least one rebase stream.
    CHECK(rebase_files > 0);

    std::filesystem::remove_all(out_dir);
    std::filesystem::remove(cuts_file);
  };

  SUBCASE("low_memory = off")
  {
    run_mode(LowMemoryMode::off);
  }

  SUBCASE("low_memory = compress")
  {
    run_mode(LowMemoryMode::compress);
  }
}
