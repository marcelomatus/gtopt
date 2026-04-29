// SPDX-License-Identifier: BSD-3-Clause
//
// Low-memory replay regression for loaded boundary cuts.
//
// `low_memory_mode = compress` is the SDDP / cascade default after
// commit 71d0da7d.  Under that mode every per-(scene, phase) backend
// is released between solves and reconstructed lazily on the next
// access.  Loaded boundary cuts are recorded by
// `LinearInterface::add_cut_row` for replay (`record_cut_row`) and
// must survive the release / reload round-trip with **bit-for-bit**
// LP-row state (RHS, coefficient at every column, row scale).
//
// This test loads a boundary CSV under non-trivial scaling
// (`scale_objective = 1000`, junction `efin` `col_scale = 5`),
// captures the post-load row state, then triggers a release →
// ensure_backend cycle and verifies the same row state survives.
// Captures any future regression where the replay path drops the row
// or re-applies / un-applies the `compose_physical` factors.

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>  // register_alpha_variables

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

[[nodiscard]] auto find_efin_col_by_uid_replay(const PlanningLP& planning_lp,
                                               SceneIndex scene_index,
                                               PhaseIndex phase_index,
                                               Uid uid)
    -> std::optional<ColIndex>
{
  const auto& svar_map =
      planning_lp.simulation().state_variables(scene_index, phase_index);
  for (const auto& [key, svar] : svar_map) {
    if (key.uid == uid && key.col_name == std::string_view {"efin"}) {
      return svar.col();
    }
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE(  // NOLINT
    "load_boundary_cuts_csv — row survives release_backend "
    "+ ensure_backend cycle under low_memory=compress")
{
  // 3-phase hydro fixture, force compress mode (the new default for
  // SDDP/cascade, but make it explicit here so the test isn't shaped
  // by environment defaults) and apply non-trivial scaling so the
  // round-trip invariant has work to do.
  auto planning = make_3phase_hydro_planning();
  planning.options.scale_objective = OptReal {1000.0};
  planning.options.variable_scales.push_back(VariableScale {
      .class_name = "Reservoir",
      .variable = "efin",
      .scale = 5.0,
  });
  // The fixture's default method is `monolithic`, which short-circuits
  // the SDDP `low_memory_mode` resolution path in PlanningLP (it
  // only fires for sddp/cascade methods; see planning_lp.cpp:586-592).
  // Switch to SDDP so the resolved `lp_matrix_options.low_memory_mode`
  // is `compress` and the per-cell LinearInterface lands in compress
  // mode — same machinery production SDDP runs exercise.
  planning.options.method = MethodType::sddp;
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::compress;
  PlanningLP planning_lp(std::move(planning));

  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / "gtopt_test_bdr_low_memory_replay.csv")
                            .string();
  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;
  {
    std::ofstream ofs(tmp_file);
    // "rsv1" maps to ("Reservoir", 1) post-P2-2; class-aware inner
    // match resolves to Reservoir:1:efin under R4.
    ofs << "name,iteration,scene,rhs,rsv1\n";
    ofs << "rt_cut,1,0," << phys_rhs << "," << phys_coeff << "\n";
  }

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);

  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto& sim = planning_lp.simulation();
  const auto last_phase = sim.last_phase_index();
  auto& last_sys = planning_lp.system(first_scene_index(), last_phase);
  auto& last_li = last_sys.linear_interface();

  const auto pre_load_numrows = last_li.get_numrows();
  const auto state_col = find_efin_col_by_uid_replay(
      planning_lp, first_scene_index(), last_phase, Uid {1});
  REQUIRE(state_col.has_value());

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);
  REQUIRE(last_li.get_numrows() == pre_load_numrows + 1);

  const auto cut_row = RowIndex {static_cast<Index>(pre_load_numrows)};

  // ── Pre-release readback ──────────────────────────────────────
  // Capture the physical-space view of the row that we'll require
  // to be identical after a release/reload cycle.
  const double pre_row_low = last_li.get_row_low()[cut_row];
  const double pre_phys_coeff = last_li.get_coeff(cut_row, *state_col);
  CAPTURE(pre_row_low);
  CAPTURE(pre_phys_coeff);
  REQUIRE(pre_row_low
          == doctest::Approx(phys_rhs).epsilon(
              PlanningOptionsLP::default_sddp_cut_coeff_eps));
  REQUIRE(pre_phys_coeff
          == doctest::Approx(-phys_coeff)
                 .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));

  // ── Release/reload cycle ──────────────────────────────────────
  last_sys.release_backend();
  REQUIRE(last_li.is_backend_released());

  // ensure_lp_built triggers the lazy reconstruction path.  The
  // recorded cut should replay from `m_active_cuts_` (compress
  // snapshot) rather than re-loaded from disk.
  last_sys.ensure_lp_built();
  REQUIRE_FALSE(last_li.is_backend_released());

  // ── Post-reload readback ───────────────────────────────────────
  // Same physical RHS, same physical coefficient, same row index —
  // the replay path must not drop, duplicate, or re-scale the cut.
  REQUIRE(last_li.get_numrows() == pre_load_numrows + 1);
  const double post_row_low = last_li.get_row_low()[cut_row];
  const double post_phys_coeff = last_li.get_coeff(cut_row, *state_col);
  CAPTURE(post_row_low);
  CAPTURE(post_phys_coeff);
  CHECK(post_row_low
        == doctest::Approx(pre_row_low)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  CHECK(post_phys_coeff
        == doctest::Approx(pre_phys_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));

  std::filesystem::remove(tmp_file);
}
