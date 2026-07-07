// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_loss_sol_emission.cpp — regression guard for the consolidated
// ``Line/loss_sol.parquet`` (= ``LP(lossp) + LP(lossn)``) extras emission
// added in 2026-05.
//
// Why
// ---
// 2026-05 replaced the paired ``Line/{lossp,lossn}_sol`` emission with a
// single ``Line/loss_sol`` stream merged over both directions (see the
// "Consolidated loss output" block in ``source/line_lp.cpp`` and the
// ``add_col_sol_extras`` overload taking ``STBIndexHolder<vector<ColIndex>>``
// in ``include/gtopt/output_context.hpp``).  Because the new path goes
// through ``add_field_sum`` rather than ``add_field`` and lives behind
// the ``extras`` gate, a missing overload, a wrong gate, or a regression
// in ``flat()`` for the sum-of-cols holder shape would silently drop the
// file without any other observable effect (other extras paths
// — overload, piecewise segments — would still pass, masking the
// breakage).  This test pins the contract by running a minimal 2-bus
// fixture with ``lossfactor > 0`` (default ``adaptive`` → ``piecewise``
// for the fixed-capacity case), calling ``SystemLP::write_out`` with
// ``extras:Line`` selected, and asserting that the CSV shard for
// ``Line/loss_sol`` is present and non-empty.

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_enums.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

// Single-phase two-bus fixture with a lossy line.  Demand sits on b2,
// generation on b1, so flow goes b1 → b2 and the LP populates
// ``lossp_cols`` (forward direction) under the default ``adaptive``
// loss mode (which resolves to ``piecewise`` for fixed-capacity lines).
[[nodiscard]] auto make_lossy_line_system()
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
      {
          .uid = Uid {2},
          .name = "b2",
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 100.0,
      },
  };
  // Force ``piecewise`` explicitly so this test is independent of the
  // global ``adaptive`` resolution.  ``piecewise`` populates both
  // ``lossp_cols`` and ``lossn_cols`` (the precondition for the
  // consolidated ``loss_sol`` emission); ``piecewise_direct`` would
  // leave them empty.
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .line_losses_mode = "piecewise",
          .loss_segments = 3,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };
  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
  const System system = {
      .name = "LossSolEmissionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };
  return std::pair {system, simulation};
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Consolidated ``loss_sol`` IS emitted when ``extras:Line`` is selected.
// Paired ``lossp_sol`` / ``lossn_sol`` MUST NOT be emitted — they were
// retired by the 2026-05 consolidation.
// ─────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LineLP - extras:Line emits consolidated Line/loss_sol shard")
{
  auto [system, simulation] = make_lossy_line_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_line_loss_sol_extras_on";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::uncompressed;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.demand_fail_cost = 1000.0;
  // ``all`` includes the ``extras`` atom; explicit form makes the
  // intent obvious at a glance.
  opts.write_out =
      OutputSelection {OutputFlags::solution | OutputFlags::extras};

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto result = system_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  system_lp.write_out();

  const auto line_dir = tmpdir / "Line";
  REQUIRE(std::filesystem::exists(line_dir));

  bool saw_loss_sol = false;
  bool saw_lossp_sol = false;
  bool saw_lossn_sol = false;
  std::filesystem::path loss_sol_path;
  for (const auto& entry : std::filesystem::directory_iterator(line_dir)) {
    const auto name = entry.path().filename().string();
    // CSV shard names follow `<field>_<tier>_s<N>_p<M>.csv` where
    // `<field>_<tier>` is the file stem and `s<N>_p<M>` is the
    // (scene, phase) shard suffix.  Match on the `loss_sol`,
    // `lossp_sol`, `lossn_sol` prefixes (the rest of the stem is the
    // shard suffix).
    if (name.starts_with("loss_sol")) {
      saw_loss_sol = true;
      loss_sol_path = entry.path();
    } else if (name.starts_with("lossp_sol")) {
      saw_lossp_sol = true;
    } else if (name.starts_with("lossn_sol")) {
      saw_lossn_sol = true;
    }
  }

  // Primary regression assertion: the consolidated stream IS emitted.
  CHECK(saw_loss_sol);
  // `lossp_sol` was retired by the consolidation — not emitted under
  // any gate.
  CHECK_FALSE(saw_lossp_sol);
  // `lossn_sol` IS emitted under the `extras` gate.  Default
  // `write_out` here is `OutputFlags::all` (includes extras), so the
  // directional B→A leg shows up alongside the consolidated
  // `loss_sol`.
  CHECK(saw_lossn_sol);

  // File must carry an actual data row, not just a header.  A broken
  // ``add_field_sum`` / ``flat()`` interaction with the
  // ``vector<ColIndex>`` value-type would short-circuit on the empty-
  // values guard and emit an empty CSV.
  if (saw_loss_sol) {
    REQUIRE(std::filesystem::file_size(loss_sol_path) > 0);
    std::ifstream in(loss_sol_path);
    REQUIRE(in.is_open());
    std::string header_line;
    std::string data_line;
    REQUIRE(std::getline(in, header_line).good());
    CHECK(std::getline(in, data_line).good());
    // Header must declare a `value` column (sum-of-cols emission).
    CHECK(header_line.find("value") != std::string::npos);
  }

  std::filesystem::remove_all(tmpdir);
}

// ─────────────────────────────────────────────────────────────────────
// ``write_out = solution`` (no ``extras``) still emits the consolidated
// ``Line/loss_sol`` shard — it now rides the ``sol`` gate, peer of
// ``flow_sol``, so any default ``write_out`` that asks for Line
// solutions also gets the loss stream.  The directional ``lossn_sol``
// remains ``extras``-gated and must NOT appear here.
// ─────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "LineLP - solution-only emits Line/loss_sol; lossn_sol stays extras-gated")
{
  auto [system, simulation] = make_lossy_line_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_line_loss_sol_solonly";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::uncompressed;
  opts.model_options.use_single_bus = false;
  opts.model_options.use_kirchhoff = false;
  opts.model_options.demand_fail_cost = 1000.0;
  // No ``extras`` atom — ``loss_sol`` rides the ``sol`` gate and
  // still appears; ``lossn_sol`` (extras-gated) must not.
  opts.write_out = OutputSelection {OutputFlags::solution};

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto result = system_lp.linear_interface().resolve();
  REQUIRE(result.has_value());
  REQUIRE(result.value() == 0);

  system_lp.write_out();

  const auto line_dir = tmpdir / "Line";
  bool saw_loss_sol = false;
  if (std::filesystem::exists(line_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(line_dir)) {
      const auto name = entry.path().filename().string();
      if (name.starts_with("loss_sol")) {
        saw_loss_sol = true;
      }
      // Retired-by-consolidation directional streams must remain off.
      CHECK_FALSE(name.starts_with("lossp_sol"));
      // The full directional / excluded-RC family is ``extras``-gated;
      // with `write_out = solution` ONLY, NONE of these may appear:
      //   * ``lossn_sol``  — B→A loss leg
      //   * ``flown_sol``  — B→A raw flow primal
      //   * ``flowe_cost`` — sign-excluded reduced cost on the flow col
      // Audit gap G10 (issue #529): the `lossn_sol` extras-gate was
      // already pinned by the test above; this widens the check to the
      // sister-extras streams introduced by the signed-flow
      // consolidation so any future regression that loosens the gate
      // on one of them is caught.
      CHECK_FALSE(name.starts_with("lossn_sol"));
      CHECK_FALSE(name.starts_with("flown_sol"));
      CHECK_FALSE(name.starts_with("flowe_cost"));
    }
  }
  CHECK(saw_loss_sol);

  std::filesystem::remove_all(tmpdir);
}
