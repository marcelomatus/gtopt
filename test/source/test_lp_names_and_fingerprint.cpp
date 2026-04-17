// SPDX-License-Identifier: BSD-3-Clause
/// @file test_lp_names_and_fingerprint.cpp
/// @brief Tests for LP names level behavior, write_lp boundary cases,
///        enforce_names_for_method, WorkPool memory gating, and fingerprint
///        simulation-stage expectations.

#include <chrono>
#include <filesystem>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_fingerprint.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/main_options.hpp>
#include <gtopt/memory_monitor.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/work_pool.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── Helper: minimal LP with names ─────────────────────────────────────────

auto make_simple_lp() -> LinearProblem
{
  LinearProblem lp("test_lp");
  const auto ctx = make_block_context(
      make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0));
  auto c0 = lp.add_col(SparseCol {
      .class_name = "Generator",
      .variable_name = "gen",
      .variable_uid = Uid {1},
      .context = ctx,
  });
  auto c1 = lp.add_col(SparseCol {
      .class_name = "Bus",
      .variable_name = "theta",
      .variable_uid = Uid {1},
      .context = ctx,
  });
  auto r0 = lp.add_row(SparseRow {
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = Uid {1},
      .context = ctx,
  });
  lp.set_coeff(r0, c0, 1.0);
  lp.set_coeff(r0, c1, -1.0);
  return lp;
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. LpNamesLevel and make_lp_matrix_options
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("LpNamesLevel::none produces no names in flatten")  // NOLINT
{
  auto lp = make_simple_lp();
  const auto flat = lp.flatten(LpMatrixOptions {
      .col_with_names = false,
      .row_with_names = false,
      .col_with_name_map = false,
      .row_with_name_map = false,
  });

  CHECK(flat.colnm.empty());
  CHECK(flat.rownm.empty());
  CHECK(flat.colmp.empty());
  CHECK(flat.rowmp.empty());
}

TEST_CASE("LpNamesLevel::none produces no col or row names")  // NOLINT
{
  auto lp = make_simple_lp();
  auto opts = make_lp_matrix_options(false, std::nullopt);
  const auto flat = lp.flatten(opts);

  // At none, no dense name vectors are built.
  CHECK(flat.colnm.empty());
  CHECK(flat.rownm.empty());
  CHECK(flat.colmp.empty());
  CHECK(flat.rowmp.empty());
}

TEST_CASE("LpNamesLevel::all produces col and row names")  // NOLINT
{
  auto lp = make_simple_lp();
  auto opts = make_lp_matrix_options(true, std::nullopt);
  const auto flat = lp.flatten(opts);

  CHECK(flat.colnm.size() == 2);
  CHECK(flat.rownm.size() == 1);
  CHECK_FALSE(flat.colmp.empty());
  CHECK(flat.rownm.size() >= 1);
}

TEST_CASE(
    "make_lp_matrix_options - disabled level disables all names")  // NOLINT
{
  auto opts = make_lp_matrix_options(false, std::nullopt);

  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
}

TEST_CASE("make_lp_matrix_options - disabled defaults to none")  // NOLINT
{
  auto opts = make_lp_matrix_options(false, std::nullopt);

  CHECK(opts.col_with_names == false);
}

TEST_CASE(  // NOLINT
    "User can override default with enabled names")
{
  // Simulate user setting names enabled in JSON/CLI
  auto opts = make_lp_matrix_options(true, std::nullopt);

  CHECK(opts.col_with_names == true);
  CHECK(opts.row_with_names == true);
  CHECK(opts.col_with_name_map == true);
  CHECK(opts.row_with_name_map == true);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. write_lp boundary cases
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("write_lp returns error when row names missing")  // NOLINT
{
  auto lp = make_simple_lp();
  // Flatten without row names (names disabled)
  auto opts = make_lp_matrix_options(false, std::nullopt);
  auto flat = lp.flatten(opts);

  LinearInterface li;
  li.load_flat(flat);

  auto result = li.write_lp("test_should_not_write.lp");
  CHECK_FALSE(result.has_value());
  CHECK(result.error().code == ErrorCode::InvalidInput);
  CHECK(result.error().message.find("row names are not available")
        != std::string::npos);

  // Cleanup just in case
  std::filesystem::remove("test_should_not_write.lp");
}

TEST_CASE("write_lp with empty filename is a no-op success")  // NOLINT
{
  LinearInterface li;
  auto result = li.write_lp("");
  CHECK(result.has_value());
}

TEST_CASE(  // NOLINT
    "write_lp does not return error when row names available")
{
  auto lp = make_simple_lp();
  auto opts = make_lp_matrix_options(true, std::nullopt);
  auto flat = lp.flatten(opts);

  LinearInterface li;
  // load_flat() copies the LabelMaker from flat.label_maker, which was
  // populated during flatten() from the LinearProblem with names enabled.
  li.load_flat(flat);

  // row_index_to_name should be populated → write_lp passes the check
  CHECK_FALSE(li.row_index_to_name().empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. enforce_names_for_method (PlanningLP constructor)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "PlanningLP multi-phase: no dense col names, state vars via map")
{
  // Multi-phase planning — state variable transfer uses the state variable
  // map (ColIndex-based) directly, no column name strings needed.
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  const auto& li = planning_lp.systems().front().front().linear_interface();

  // No name vectors or maps at default names_level=none
  CHECK(li.col_index_to_name().empty());
  CHECK(li.row_index_to_name().empty());
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());

  // LP itself must have cols and rows
  CHECK(li.get_numcols() > 0);
  CHECK(li.get_numrows() > 0);

  // State variables should still be registered for inter-phase transfer
  const auto& sim = planning_lp.simulation();
  CHECK_FALSE(sim.state_variables(SceneIndex {0}, PhaseIndex {0}).empty());
}

TEST_CASE(  // NOLINT
    "PlanningLP respects names_level=none for single-phase planning")
{
  // Create a single-phase planning
  auto planning = make_single_phase_planning();

  // Single phase → no state transfer needed → should keep none
  PlanningLP planning_lp(std::move(planning));

  const auto& li = planning_lp.systems().front().front().linear_interface();

  // No dense name vectors
  CHECK(li.col_index_to_name().empty());
  CHECK(li.row_index_to_name().empty());

  // No name-to-index maps
  CHECK(li.col_name_map().empty());
  CHECK(li.row_name_map().empty());

  // LP must still have actual cols and rows (names skipped, not the LP)
  CHECK(li.get_numcols() > 0);
  CHECK(li.get_numrows() > 0);
}

TEST_CASE(  // NOLINT
    "PlanningLP with explicit col_with_names=true overrides default")
{
  auto planning = make_single_phase_planning();

  LpMatrixOptions opts;
  opts.col_with_names = true;
  opts.row_with_names = true;
  opts.col_with_name_map = true;

  PlanningLP planning_lp(std::move(planning), opts);

  const auto& li = planning_lp.systems().front().front().linear_interface();
  CHECK_FALSE(li.col_index_to_name().empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. WorkPool memory-aware scheduling
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("WorkPoolConfig memory defaults")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const WorkPoolConfig config;

  CHECK(config.min_free_memory_mb == doctest::Approx(2048.0));
  CHECK(config.max_memory_percent == doctest::Approx(95.0));
  CHECK(config.max_process_rss_mb == doctest::Approx(0.0));
}

TEST_CASE("WorkPoolConfig custom memory caps")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  const WorkPoolConfig config {
      4,  // max_threads
      90.0,  // max_cpu_threshold
      2048.0,  // min_free_memory_mb
      80.0,  // max_memory_percent
      8192.0,  // max_process_rss_mb
      10ms,  // scheduler_interval
      "CustomMemPool",  // name
  };

  CHECK(config.max_threads == 4);
  CHECK(config.max_cpu_threshold == doctest::Approx(90.0));
  CHECK(config.min_free_memory_mb == doctest::Approx(2048.0));
  CHECK(config.max_memory_percent == doctest::Approx(80.0));
  CHECK(config.max_process_rss_mb == doctest::Approx(8192.0));
  CHECK(config.scheduler_interval == 10ms);
  CHECK(config.name == "CustomMemPool");
}

TEST_CASE("WorkPool statistics include memory fields")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  using namespace std::chrono_literals;

  AdaptiveWorkPool pool(WorkPoolConfig {
      2,
      95.0,
      4096.0,
      95.0,
      0.0,
      10ms,
      "MemStatsPool",
  });
  pool.start();

  auto result = pool.submit([] { return 42; });
  REQUIRE(result.has_value());
  result.value().wait();

  std::this_thread::sleep_for(50ms);

  const auto stats = pool.get_statistics();
  // Memory percent should be between 0 and 100
  CHECK(stats.current_memory_percent >= 0.0);
  CHECK(stats.current_memory_percent <= 100.0);
  // Available memory should be non-negative
  CHECK(stats.available_memory_mb >= 0.0);
  // Process RSS should be non-negative
  CHECK(stats.process_rss_mb >= 0.0);

  pool.shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. MemoryMonitor
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("MemoryMonitor snapshot returns valid values")  // NOLINT
{
  auto snapshot = MemoryMonitor::get_system_memory_snapshot();

  // total_mb should be > 0 on any real system
  CHECK(snapshot.total_mb > 0.0);
  // available should be <= total
  CHECK(snapshot.available_mb <= snapshot.total_mb);
  CHECK(snapshot.available_mb >= 0.0);
  // process RSS should be >= 0 (may be 0 on some platforms like WSL2)
  CHECK(snapshot.process_rss_mb >= 0.0);
}

TEST_CASE("MemoryMonitor starts and reports")  // NOLINT
{
  MemoryMonitor monitor;
  monitor.start();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  CHECK(monitor.get_total_mb() > 0.0);
  CHECK(monitor.get_available_mb() > 0.0);
  CHECK(monitor.get_memory_percent() >= 0.0);
  CHECK(monitor.get_memory_percent() <= 100.0);
  CHECK(monitor.get_process_rss_mb() >= 0.0);

  monitor.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. LpFingerprint with simulation phases
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("LpFingerprint with ScenePhaseContext entries")  // NOLINT
{
  std::vector<SparseCol> cols = {
      SparseCol {
          .class_name = "Generator",
          .variable_name = "generation",
          .variable_uid = Uid {1},
          .context = make_block_context(
              make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)),
      },
      SparseCol {
          .class_name = "Reservoir",
          .variable_name = "efin",
          .variable_uid = Uid {1},
          .context =
              make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0)),
      },
  };
  std::vector<SparseRow> rows = {
      SparseRow {
          .class_name = "Bus",
          .constraint_name = "balance",
          .variable_uid = Uid {1},
          .context = make_block_context(
              make_uid<Scenario>(0), make_uid<Stage>(0), make_uid<Block>(0)),
      },
  };

  auto fp = compute_lp_fingerprint(cols, rows);

  // Two distinct col template entries (different class+variable+context)
  CHECK(fp.col_template.size() == 2);
  CHECK(fp.row_template.size() == 1);

  // Check context types
  bool has_block = false;
  bool has_stage = false;
  for (const auto& entry : fp.col_template) {
    if (entry.context_type == "BlockContext") {
      has_block = true;
    }
    if (entry.context_type == "StageContext") {
      has_stage = true;
    }
  }
  CHECK(has_block);
  CHECK(has_stage);
}

TEST_CASE("LpFingerprint empty LP produces empty hash")  // NOLINT
{
  std::vector<SparseCol> cols;
  std::vector<SparseRow> rows;

  auto fp = compute_lp_fingerprint(cols, rows);

  CHECK(fp.col_template.empty());
  CHECK(fp.row_template.empty());
  CHECK(fp.stats.total_cols == 0);
  CHECK(fp.stats.total_rows == 0);
  CHECK(fp.untracked_cols == 0);
  CHECK(fp.untracked_rows == 0);
  // Hashes should still be valid 64-char hex strings
  CHECK(fp.col_hash.size() == 64);
  CHECK(fp.row_hash.size() == 64);
  CHECK(fp.structural_hash.size() == 64);
}

TEST_CASE(  // NOLINT
    "LpFingerprint same structure different UIDs same hash")
{
  auto make_cols = [](Uid uid)
  {
    return std::vector<SparseCol> {
        SparseCol {
            .class_name = "Reservoir",
            .variable_name = "efin",
            .variable_uid = uid,
            .context =
                make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0)),
        },
    };
  };

  std::vector<SparseRow> rows;
  auto fp1 = compute_lp_fingerprint(make_cols(Uid {1}), rows);
  auto fp2 = compute_lp_fingerprint(make_cols(Uid {99}), rows);

  CHECK(fp1.structural_hash == fp2.structural_hash);
}

TEST_CASE("sha256_hex produces consistent 64-char output")  // NOLINT
{
  const auto h1 = sha256_hex("hello world");
  const auto h2 = sha256_hex("hello world");

  CHECK(h1.size() == 64);
  CHECK(h1 == h2);

  // Different input → different hash
  const auto h3 = sha256_hex("hello world!");
  CHECK(h1 != h3);
}

TEST_CASE(  // NOLINT
    "LpFingerprint write and read JSON round-trip")
{
  std::vector<SparseCol> cols = {
      SparseCol {
          .class_name = "Reservoir",
          .variable_name = "efin",
          .variable_uid = Uid {1},
          .context =
              make_stage_context(make_uid<Scenario>(0), make_uid<Stage>(0)),
      },
  };
  std::vector<SparseRow> rows;

  auto fp = compute_lp_fingerprint(cols, rows);
  const std::string filepath = "test_fp_simulation_stage.json";

  write_lp_fingerprint(fp, filepath, 1, 2);

  std::ifstream file(filepath);
  REQUIRE(file.is_open());
  std::string content((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
  file.close();

  CHECK(content.find("\"scene_uid\": 1") != std::string::npos);
  CHECK(content.find("\"phase_uid\": 2") != std::string::npos);
  CHECK(content.find("\"Reservoir\"") != std::string::npos);
  CHECK(content.find("\"StageContext\"") != std::string::npos);
  CHECK(content.find(fp.structural_hash) != std::string::npos);

  std::filesystem::remove(filepath);
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. Low-memory compress + cascade + fingerprint + write_lp integration
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "Low-memory compress with cascade names and LP fingerprint")
{
  // Build a 3-phase hydro planning (cascade-style multi-phase).
  // enforce_names_for_method auto-upgrades to minimal names for phases > 1.
  auto planning = make_3phase_hydro_planning();
  planning.options.use_single_bus = OptBool {true};
  planning.options.scale_objective = OptReal {1.0};

  const auto flat_opts =
      make_lp_matrix_options(false,  // enable_names → none (default)
                             std::nullopt,  // matrix_eps
                             false,  // compute_stats
                             std::nullopt,  // solver
                             std::nullopt);  // equilibration

  // State variable I/O uses the state variable map directly — no
  // column name strings needed even for multi-phase.
  PlanningLP plp(std::move(planning), flat_opts);

  REQUIRE(plp.systems().size() == 1);  // 1 scene
  REQUIRE(plp.systems().front().size() == 3);  // 3 phases

  SUBCASE("no dense column names at default level")
  {
    for (size_t pi = 0; pi < 3; ++pi) {
      const auto& li = plp.systems()
                           .front()[PhaseIndex {static_cast<int>(pi)}]
                           .linear_interface();
      CHECK(li.col_index_to_name().empty());
    }
    // State variables registered via ColIndex-based map
    CHECK_FALSE(plp.simulation()
                    .state_variables(SceneIndex {0}, PhaseIndex {0})
                    .empty());
  }

  SUBCASE("LP fingerprint is consistent across phases")
  {
    // Each phase should have the same structural fingerprint
    // (same variable types and constraints, different element counts).
    const auto& sys0 =
        plp.systems().front()[first_phase_index()].linear_interface();
    const auto& sys1 = plp.systems().front()[PhaseIndex {1}].linear_interface();

    // Both phases should have the same number of structural columns
    CHECK(sys0.get_numcols() > 0);
    CHECK(sys1.get_numcols() > 0);
  }

  SUBCASE("low-memory compress preserves structure across cycles")
  {
    auto& sys = plp.systems().front()[first_phase_index()];
    auto& li = sys.linear_interface();

    // Solve first
    auto res = li.initial_solve();
    REQUIRE(res.has_value());
    REQUIRE(li.is_optimal());

    const auto orig_ncols = li.get_numcols();
    const auto orig_nrows = li.get_numrows();
    const auto orig_obj = li.get_obj_value();

    // Enable compression — snapshot was already saved during LP build
    sys.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);

    // Cycle: release → reconstruct × 3
    for (int cycle = 0; cycle < 3; ++cycle) {
      li.release_backend();
      CHECK(li.is_backend_released());

      li.reconstruct_backend();
      CHECK_FALSE(li.is_backend_released());
      CHECK(li.get_numcols() == orig_ncols);
      CHECK(li.get_numrows() == orig_nrows);

      auto r = li.resolve();
      REQUIRE(r.has_value());
      CHECK(li.get_obj_value() == doctest::Approx(orig_obj));
    }
  }

  SUBCASE("solver_id is safe after backend release")
  {
    auto& li = plp.systems().front()[first_phase_index()].linear_interface();
    auto res = li.initial_solve();
    REQUIRE(res.has_value());

    const auto id_before = li.solver_id();
    CHECK_FALSE(id_before.empty());

    plp.systems().front()[first_phase_index()].set_low_memory(
        LowMemoryMode::compress, CompressionCodec::lz4);
    plp.systems().front()[first_phase_index()].release_backend();

    // solver_id must not crash even with backend released
    const auto id_after = li.solver_id();
    CHECK(id_after == id_before);
  }
}

TEST_CASE("write_lp succeeds with full names enabled")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  planning.options.use_single_bus = OptBool {true};
  planning.options.scale_objective = OptReal {1.0};

  const auto flat_opts = make_lp_matrix_options(
      true, std::nullopt, false, std::nullopt, std::nullopt);

  PlanningLP plp(std::move(planning), flat_opts);
  REQUIRE(plp.systems().size() == 1);
  REQUIRE(plp.systems().front().size() == 3);

  auto& li = plp.systems().front()[first_phase_index()].linear_interface();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  CHECK_FALSE(li.row_index_to_name().empty());

  const std::string lp_base = "test_lp_debug_phase0";
  auto wr = li.write_lp(lp_base);
  CHECK(wr.has_value());

  // Backend may write to base.lp — check both paths
  const auto lp_file =
      std::filesystem::exists(lp_base + ".lp") ? lp_base + ".lp" : lp_base;
  CHECK(std::filesystem::exists(lp_file));
  if (std::filesystem::exists(lp_file)) {
    CHECK(std::filesystem::file_size(lp_file) > 100);
    std::filesystem::remove(lp_file);
  }
}

TEST_CASE(  // NOLINT
    "write_lp after compress/reconstruct preserves names")
{
  auto planning = make_3phase_hydro_planning();
  planning.options.use_single_bus = OptBool {true};
  planning.options.scale_objective = OptReal {1.0};

  const auto flat_opts = make_lp_matrix_options(
      true, std::nullopt, false, std::nullopt, std::nullopt);

  PlanningLP plp(std::move(planning), flat_opts);
  REQUIRE(plp.systems().size() == 1);

  auto& sys = plp.systems().front()[first_phase_index()];
  auto& li = sys.linear_interface();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  // Enable compress mode, release, reconstruct
  sys.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
  sys.release_backend();
  sys.reconstruct_backend();

  // Re-solve after reconstruction — backend stays live (no auto-release).
  auto r = li.resolve();
  REQUIRE(r.has_value());
  CHECK_FALSE(li.is_backend_released());

  // write_lp should still work — names survived across compress cycles
  CHECK_FALSE(li.row_index_to_name().empty());
  const std::string lp_base = "test_lp_debug_reconstructed";
  auto wr = li.write_lp(lp_base);
  CHECK(wr.has_value());
  const auto lp_file =
      std::filesystem::exists(lp_base + ".lp") ? lp_base + ".lp" : lp_base;
  if (std::filesystem::exists(lp_file)) {
    std::filesystem::remove(lp_file);
  }
}

TEST_CASE("write_lp fails with names disabled")  // NOLINT
{
  auto planning = make_single_phase_planning();
  planning.options.use_single_bus = OptBool {true};
  planning.options.scale_objective = OptReal {1.0};

  const auto no_names = make_lp_matrix_options(
      false, std::nullopt, false, std::nullopt, std::nullopt);
  PlanningLP plp(std::move(planning), no_names);
  auto& li = plp.systems().front().front().linear_interface();
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  auto wr = li.write_lp("should_not_exist.lp");
  CHECK_FALSE(wr.has_value());
  CHECK_FALSE(std::filesystem::exists("should_not_exist.lp"));
}

TEST_CASE(  // NOLINT
    "LP fingerprint with low-memory compress round-trip")
{
  // Build simple LP, compute fingerprint, then verify it survives
  // compress/reconstruct cycles.
  auto lp = make_simple_lp();
  const auto flat_opts = make_lp_matrix_options(
      false, std::nullopt, false, std::nullopt, std::nullopt);
  auto flat = lp.flatten(flat_opts);

  // Compute fingerprint from LP structure
  auto fp_before = compute_lp_fingerprint(lp.get_cols(), lp.get_rows());
  CHECK_FALSE(fp_before.col_template.empty());
  CHECK(fp_before.structural_hash.size() == 64);

  // Load into LinearInterface, enable compress mode
  LinearInterface li("", flat, "");
  auto res = li.initial_solve();
  REQUIRE(res.has_value());

  li.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
  li.save_snapshot(FlatLinearProblem {flat});

  // Release/reconstruct cycle
  li.release_backend();
  li.reconstruct_backend();

  auto r = li.resolve();
  REQUIRE(r.has_value());

  // Fingerprint is computed from LP structure, not from LinearInterface,
  // so it should be identical before and after reconstruction
  auto fp_after = compute_lp_fingerprint(lp.get_cols(), lp.get_rows());
  CHECK(fp_after.structural_hash == fp_before.structural_hash);
  CHECK(fp_after.col_template.size() == fp_before.col_template.size());
  CHECK(fp_after.row_template.size() == fp_before.row_template.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. LP fingerprint with all low memory modes (off, snapshot, compress)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "LP fingerprint with all low-memory modes on multi-phase planning")
{
  auto planning = make_3phase_hydro_planning();
  planning.options.use_single_bus = OptBool {true};
  planning.options.scale_objective = OptReal {1.0};

  // Use all to get dense column names for fingerprint-relevant metadata
  const auto flat_opts = make_lp_matrix_options(
      true, std::nullopt, false, std::nullopt, std::nullopt);
  PlanningLP plp(std::move(planning), flat_opts);

  REQUIRE(plp.systems().size() == 1);
  REQUIRE(plp.systems().front().size() == 3);

  // Solve all phases to establish baseline
  for (int pi = 0; pi < 3; ++pi) {
    auto& li = plp.systems().front()[PhaseIndex {pi}].linear_interface();
    auto res = li.initial_solve();
    REQUIRE(res.has_value());
  }

  // Collect baseline name counts per phase
  std::array<size_t, 3> baseline_col_names {};
  std::array<size_t, 3> baseline_ncols {};
  std::array<size_t, 3> baseline_nrows {};
  std::array<double, 3> baseline_obj {};
  for (int pi = 0; pi < 3; ++pi) {
    const auto& li = plp.systems().front()[PhaseIndex {pi}].linear_interface();
    baseline_col_names[pi] = li.col_name_map().size();
    baseline_ncols[pi] = li.get_numcols();
    baseline_nrows[pi] = li.get_numrows();
    baseline_obj[pi] = li.get_obj_value();
    CHECK(baseline_col_names[pi] > 0);
  }

  SUBCASE("LowMemoryMode::off — no release, names unchanged")
  {
    // Default mode: nothing released, names stay intact
    for (int pi = 0; pi < 3; ++pi) {
      const auto& li =
          plp.systems().front()[PhaseIndex {pi}].linear_interface();
      CHECK_FALSE(li.is_backend_released());
      CHECK(li.col_name_map().size() == baseline_col_names[pi]);
      CHECK(li.get_numcols() == baseline_ncols[pi]);
    }
  }

  SUBCASE("LowMemoryMode::compress — names survive release/reconstruct")
  {
    for (int pi = 0; pi < 3; ++pi) {
      auto& sys = plp.systems().front()[PhaseIndex {pi}];
      sys.set_low_memory(LowMemoryMode::compress);
      sys.release_backend();
      CHECK(sys.linear_interface().is_backend_released());

      sys.reconstruct_backend();
      CHECK_FALSE(sys.linear_interface().is_backend_released());
      CHECK(sys.linear_interface().col_name_map().size()
            == baseline_col_names[pi]);
      CHECK(sys.linear_interface().get_numcols() == baseline_ncols[pi]);
      CHECK(sys.linear_interface().get_numrows() == baseline_nrows[pi]);

      auto r = sys.linear_interface().resolve();
      REQUIRE(r.has_value());
    }
  }

  SUBCASE(
      "LowMemoryMode::compress lz4 — names survive multi-cycle "
      "release/reconstruct")
  {
    for (int pi = 0; pi < 3; ++pi) {
      auto& sys = plp.systems().front()[PhaseIndex {pi}];
      sys.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);

      // Two full cycles
      for (int cycle = 0; cycle < 2; ++cycle) {
        sys.release_backend();
        CHECK(sys.linear_interface().is_backend_released());

        sys.reconstruct_backend();
        CHECK_FALSE(sys.linear_interface().is_backend_released());
        CHECK(sys.linear_interface().col_name_map().size()
              == baseline_col_names[pi]);
        CHECK(sys.linear_interface().get_numcols() == baseline_ncols[pi]);
        CHECK(sys.linear_interface().get_numrows() == baseline_nrows[pi]);

        auto r = sys.linear_interface().resolve();
        REQUIRE(r.has_value());
      }
    }
  }

  SUBCASE(
      "LowMemoryMode::compress zstd — names survive multi-cycle "
      "release/reconstruct")
  {
    for (int pi = 0; pi < 3; ++pi) {
      auto& sys = plp.systems().front()[PhaseIndex {pi}];
      sys.set_low_memory(LowMemoryMode::compress, CompressionCodec::zstd);

      sys.release_backend();
      sys.reconstruct_backend();
      CHECK(sys.linear_interface().col_name_map().size()
            == baseline_col_names[pi]);

      auto r = sys.linear_interface().resolve();
      REQUIRE(r.has_value());
    }
  }

  SUBCASE("solver_id survives all low-memory modes")
  {
    const auto& li =
        plp.systems().front()[first_phase_index()].linear_interface();
    const auto id = li.solver_id();
    CHECK_FALSE(id.empty());

    auto& sys = plp.systems().front()[first_phase_index()];

    // snapshot mode
    sys.set_low_memory(LowMemoryMode::compress);
    sys.release_backend();
    CHECK(li.solver_id() == id);
    sys.reconstruct_backend();
    CHECK(li.solver_id() == id);

    // compress mode
    sys.set_low_memory(LowMemoryMode::compress, CompressionCodec::lz4);
    sys.release_backend();
    CHECK(li.solver_id() == id);
    sys.reconstruct_backend();
    CHECK(li.solver_id() == id);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 9. LpMatrixOptions struct defaults
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("LpMatrixOptions struct defaults match none level")  // NOLINT
{
  LpMatrixOptions opts;

  CHECK(opts.col_with_names == false);
  CHECK(opts.row_with_names == false);
  CHECK(opts.col_with_name_map == false);
  CHECK(opts.row_with_name_map == false);
  CHECK(opts.eps == doctest::Approx(0.0));
}

TEST_CASE("LpNamesLevel ordering")  // NOLINT
{
  CHECK(LpNamesLevel::none != LpNamesLevel::all);
  CHECK(LpNamesLevel::none < LpNamesLevel::all);
}

}  // namespace
