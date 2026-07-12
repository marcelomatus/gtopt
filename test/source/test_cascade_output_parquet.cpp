// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_cascade_output_parquet.cpp
 * @brief     End-to-end: cascade.solve() → write_out() → parquet round-trip
 * @date      2026-05-16
 * @copyright BSD-3-Clause
 *
 * Regression guard for the 2026-05 bug where the cascade's last active
 * level was silently skipping its simulation pass under async aggregate
 * convergence — the `m_stop_requested_` drain transitioned every idle
 * scene straight to `SceneState::done`, bypassing the `scene_finished`
 * branch that submits `run_scene_simulation`.  Symptom on disk: every
 * `*_dual.parquet` carried only NaN, and `generation_sol.parquet`
 * carried only the last-training-forward primal values (no crossover
 * duals because training-pass forward solves run with `crossover=false`).
 *
 * Strategy
 * --------
 * Run a tiny two-level cascade on the canonical 3-phase / single-bus
 * hydro fixture, redirect output to a tmpdir as Parquet, call
 * `planning_lp.write_out()`, then read the resulting `Generator`,
 * `Demand` and `Bus` parquets back via `parquet::arrow::FileReader`
 * and assert:
 *
 *   1. `Generator/generation_sol.parquet` — total dispatch per block
 *      equals demand on every active block.  Failure means the LP
 *      either crashed (no shard) or wrote partial data.
 *   2. `Demand/load_sol.parquet` — *served* demand per block equals
 *      the demand capacity (100 MW) for every active block.
 *      Combined with the generator-side check this proves the LP
 *      balance row was honoured end-to-end.
 *   3. `Bus/balance_dual.parquet` — exists and carries finite values
 *      (NOT all-NaN) — proof that the **simulation pass** ran with
 *      `crossover=true`.  Training forward passes set
 *      `crossover=false`, so row duals are unset and the output
 *      file would be all-NaN if the sim pass were skipped.
 *
 * The whole sequence is parameterised over the
 * (low_memory_mode, max_iterations) cross-product:
 *
 *   * `LowMemoryMode::off` — backend stays resident; write_out
 *     reads the still-live LP.
 *   * `LowMemoryMode::compress` — backend is released between
 *     solves; write_out has to rehydrate the LP from the
 *     compressed snapshot.  This is the path the 2026-05
 *     drain-skip bug actually surfaced under (juan/IPLP uses
 *     `compress` by default).
 *
 *   * `max_iterations` 0 / 1 / 5 — bracket the spectrum from
 *     "no training at all, only sim pass" up to "enough cuts to
 *     converge before max_iter".  All three must produce the
 *     same served-demand answer because the network is small
 *     enough to be optimal at every cut-count level (50 MW hydro
 *     + 500 MW thermal vs 100 MW demand — far from binding).
 *
 * This test exercises the failure mode caught only by reading the
 * parquet payload, not by checking the in-memory `level_stats` or
 * the `result->size()` — the latter both reported a healthy converged
 * run while the on-disk output was meaningless.
 */

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <arrow/io/file.h>
#include <arrow/table.h>
#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/solver_stats.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/reader.h>

#include "cascade_helpers.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

// Read one (scene, phase) shard of a hive-partitioned parquet output
// directory.  Returns nullptr if the part file is missing — caller
// asserts on the result.
[[nodiscard]] auto read_parquet_shard(const std::filesystem::path& dataset,
                                      gtopt::uid_t scene_uid,
                                      gtopt::uid_t phase_uid)
    -> std::shared_ptr<arrow::Table>
{
  const auto path = dataset / std::format("scene={}", scene_uid)
      / std::format("phase={}", phase_uid) / "part.parquet";
  if (!std::filesystem::exists(path)) {
    return nullptr;
  }
  auto input_stream_result = arrow::io::ReadableFile::Open(path.string());
  if (!input_stream_result.ok()) {
    return nullptr;
  }
  auto parquet_reader = parquet::ParquetFileReader::Open(*input_stream_result);
  std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
  const auto reader_status = parquet::arrow::FileReader::Make(
      arrow::default_memory_pool(), std::move(parquet_reader), &arrow_reader);
  if (!reader_status.ok()) {
    return nullptr;
  }
  std::shared_ptr<arrow::Table> table;
  const auto read_status = arrow_reader->ReadTable(&table);
  if (!read_status.ok()) {
    return nullptr;
  }
  return table;
}

// Read an Arrow numeric array element as double (handles the uint16 keys and
// the float32 / float64 value column produced by the long writer).
[[nodiscard]] auto numeric_at(const std::shared_ptr<arrow::Array>& a, int64_t i)
    -> double
{
  switch (a->type_id()) {
    case arrow::Type::FLOAT:
      return std::static_pointer_cast<arrow::FloatArray>(a)->Value(i);
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(a)->Value(i);
    case arrow::Type::UINT16:
      return std::static_pointer_cast<arrow::UInt16Array>(a)->Value(i);
    case arrow::Type::INT32:
      return std::static_pointer_cast<arrow::Int32Array>(a)->Value(i);
    case arrow::Type::INT64:
      return static_cast<double>(
          std::static_pointer_cast<arrow::Int64Array>(a)->Value(i));
    default:
      return 0.0;
  }
}

// Count finite (non-NaN, non-Inf) entries in the long `value` column.  Used
// to distinguish "sim pass ran" (finite duals) from "sim pass skipped".
[[nodiscard]] auto count_finite_doubles(const arrow::Table& table)
    -> std::size_t
{
  const auto idx = table.schema()->GetFieldIndex("value");
  if (idx < 0) {
    return 0;
  }
  const auto& col = table.column(idx);
  std::size_t finite_count = 0;
  for (int ch = 0; ch < col->num_chunks(); ++ch) {
    const auto chunk = col->chunk(ch);
    for (int64_t i = 0; i < chunk->length(); ++i) {
      if (!chunk->IsNull(i) && std::isfinite(numeric_at(chunk, i))) {
        ++finite_count;
      }
    }
  }
  return finite_count;
}

// Sum the long `value` column grouped by (scenario, stage, block).  Inactive
// cells have no rows (zeros dropped), so the returned vector holds one total
// per distinct cell present in the file — the long counterpart of the old
// per-row sum over wide `uid:N` columns.
[[nodiscard]] auto sum_per_cell(const arrow::Table& table)
    -> std::vector<double>
{
  const auto sidx = table.schema()->GetFieldIndex("scenario");
  const auto tidx = table.schema()->GetFieldIndex("stage");
  const auto bidx = table.schema()->GetFieldIndex("block");
  const auto vidx = table.schema()->GetFieldIndex("value");
  if (sidx < 0 || tidx < 0 || bidx < 0 || vidx < 0) {
    return {};
  }
  const auto combined = table.CombineChunks().ValueOr(nullptr);
  if (!combined) {
    return {};
  }
  const auto s = combined->column(sidx)->chunk(0);
  const auto t = combined->column(tidx)->chunk(0);
  const auto b = combined->column(bidx)->chunk(0);
  const auto v = combined->column(vidx)->chunk(0);
  std::map<std::tuple<int64_t, int64_t, int64_t>, double> acc;
  for (int64_t i = 0; i < (v ? v->length() : 0); ++i) {
    if (v->IsNull(i)) {
      continue;
    }
    const std::tuple<int64_t, int64_t, int64_t> key {
        static_cast<int64_t>(numeric_at(s, i)),
        static_cast<int64_t>(numeric_at(t, i)),
        static_cast<int64_t>(numeric_at(b, i)),
    };
    acc[key] += numeric_at(v, i);
  }
  std::vector<double> totals;
  totals.reserve(acc.size());
  for (const auto& [k, total] : acc) {
    totals.push_back(total);
  }
  return totals;
}

/// Sum `total_solve_calls()` across every (scene, phase) cell of the
/// planning_lp's grid, following the cascade output delegate when
/// the caller's own systems have been released.  Local replica of the
/// `gtopt_lp_runner.cpp::aggregate_solver_stats` helper — kept inline
/// so the test does not have to expose the runner-internal aggregator.
[[nodiscard]] auto count_total_solve_calls(const PlanningLP& planning_lp)
    -> std::size_t
{
  const PlanningLP* owner = &planning_lp;
  if (owner->systems().empty() && owner->output_delegate() != nullptr) {
    owner = owner->output_delegate();
  }
  std::size_t calls = 0;
  for (const auto& phase_systems : owner->systems()) {
    for (const auto& sys : phase_systems) {
      calls += sys.solver_stats().total_solve_calls();
    }
  }
  return calls;
}

// One end-to-end (build → solve → write_out → read-back) check at a
// given low-memory mode + max_iterations.  Asserts on:
//   * Σ generation == demand per active block (both sides of the
//     bus balance row).
//   * Σ served load (`load_sol`) == demand capacity per active block.
//   * Bus balance duals are finite for ≥ half of active blocks
//     (sim pass with crossover ran).
//   * `level_stats().back().iterations` matches the cascade async
//     iter-count fix (>0 when max_iters >= 1).
//   * `total_solve_calls()` aggregated via the output delegate is > 0
//     when training ran — regression guard for the
//     `aggregate_solver_stats` delegate-follow fix.
//   * `level_stats().back().have_gap_delta == true` when the level
//     produced >= 2 training iterations.
auto run_cascade_and_check_outputs(LowMemoryMode memory_mode, int max_iters)
    -> void
{
  CAPTURE(enum_name(memory_mode));
  CAPTURE(max_iters);

  // Per-case tmpdir so concurrent SUBCASEs (and parallel test
  // discovery) cannot stomp on each other's output.
  const auto tmpdir = std::filesystem::temp_directory_path()
      / std::format("gtopt_cascade_parquet_test_{}_iter{}",
                    enum_name(memory_mode),
                    max_iters);
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  // ── Planning fixture: 3-phase hydro, single bus, single scenario ─
  auto planning = make_3phase_hydro_planning();
  planning.options.output_directory = tmpdir.string();
  planning.options.output_format = DataFormat::parquet;
  // Long output: per-cell totals come from a (scenario, stage, block)
  // GROUP BY over the single `value` column (zero rows dropped), so the
  // correctness assertions below count balanced *cells*, not raw rows.
  PlanningLP planning_lp(std::move(planning));

  // ── Two-level cascade.  Each level inherits `max_iters` from the
  // outer parameterisation so the test exercises:
  //   max_iters=0 → no training, sim pass only (the most fragile
  //                  path; sim must still produce a feasible LP from
  //                  the initial α-bounds alone).
  //   max_iters=1 → 1 training iter then sim.
  //   max_iters=5 → enough iters to converge for this small fixture.
  // For every value the served-demand answer must be the same (the
  // network is far from binding).
  SDDPOptions base_opts;
  base_opts.max_iterations = max_iters;
  base_opts.min_iterations =
      0;  // allow max_iters=0 to bypass training entirely
  base_opts.convergence_tol = 0.01;
  base_opts.apertures = std::vector<Uid> {};
  base_opts.enable_api = false;
  base_opts.low_memory_mode = memory_mode;

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"warmup"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {max_iters},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"final"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {max_iters},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base_opts), std::move(cascade));
  const SolverOptions lp_opts;
  const auto res = solver.solve(planning_lp, lp_opts);
  REQUIRE(res.has_value());
  REQUIRE(solver.level_stats().size() == 2);

  // ── B. Cascade async iter-count regression ─────────────────────
  // Without the `sim_entry_in_result` correction the final-level
  // count underflows to `0` whenever the SDDP path goes through
  // the async dispatch (cut_sharing=none && max_async_spread > 0),
  // even when training ran for many iterations.  Here the small
  // fixture uses the default solve path; the invariant we pin is
  // `iters == 0` exactly when `max_iters == 0`, otherwise > 0.
  const auto& last_level = solver.level_stats().back();
  if (max_iters == 0) {
    CHECK(last_level.iterations == 0);
  } else {
    CHECK(last_level.iterations > 0);
  }

  // ── C. Aggregated solver_stats follows the output delegate ─────
  // In cascade mode `planning_lp.systems()` is empty after the
  // level-0→1 transition; the active grid lives in
  // `output_delegate()`.  Without the runner's
  // `resolve_owning_lp(...)` fix the summary printed "solves: 0".
  // Mirror that walk locally and assert > 0 when training ran.
  const auto total_solves = count_total_solve_calls(planning_lp);
  if (max_iters >= 1) {
    CHECK(total_solves > 0);
  }

  // ── D. gap_delta storage ───────────────────────────────────────
  // Populated when `result->size() >= 2` (i.e. at least two training
  // iters fed the level summary).  With `max_iters >= 2` the level
  // always trains at least twice on this fixture.
  if (max_iters >= 2) {
    CHECK(last_level.have_gap_delta);
  }

  planning_lp.write_out();

  // OutputContext partitions on the SCENE uid (0-indexed; SceneLP
  // is built from scenario_array grouping), not the scenario uid.
  // The 3-phase hydro fixture has one scenario folded into scene 0.
  // Phase UIDs are 1 / 2 / 3 (as declared in phase_array).
  constexpr gtopt::uid_t scene_uid = 0;
  constexpr std::array<gtopt::uid_t, 3> phase_uids = {1, 2, 3};
  // Demand capacity from the fixture: 1 demand × 100 MW × 24 active
  // blocks per phase × 3 phases = 7200 MWh balanced cells.
  constexpr double expected_demand_mw = 100.0;
  constexpr std::size_t active_blocks_per_phase = 24;
  constexpr std::size_t expected_balanced_cells =
      active_blocks_per_phase * phase_uids.size();

  // ── 1. Generation balance: Σ generation == demand per block ────
  const auto gen_sol_root =
      std::filesystem::path(tmpdir) / "Generator" / "generation_sol.parquet";
  REQUIRE(std::filesystem::exists(gen_sol_root));

  std::size_t gen_balanced_blocks = 0;
  for (const auto phase_uid : phase_uids) {
    CAPTURE(phase_uid);
    auto gen_sol = read_parquet_shard(gen_sol_root, scene_uid, phase_uid);
    REQUIRE(gen_sol != nullptr);
    for (const auto total : sum_per_cell(*gen_sol)) {
      if (total > 1e-6) {
        CHECK(total == doctest::Approx(expected_demand_mw).epsilon(1e-3));
        ++gen_balanced_blocks;
      }
    }
  }
  CHECK(gen_balanced_blocks == expected_balanced_cells);

  // ── 2. Served demand: load_sol == demand capacity per block ────
  // load_sol = (demand capacity) − fail_sol.  With ~unlimited
  // thermal headroom the LP must dispatch the full 100 MW every
  // active block — fail_sol should be exactly zero.
  const auto load_sol_root =
      std::filesystem::path(tmpdir) / "Demand" / "load_sol.parquet";
  REQUIRE(std::filesystem::exists(load_sol_root));

  std::size_t load_balanced_blocks = 0;
  for (const auto phase_uid : phase_uids) {
    CAPTURE(phase_uid);
    auto load_sol = read_parquet_shard(load_sol_root, scene_uid, phase_uid);
    REQUIRE(load_sol != nullptr);
    for (const auto served : sum_per_cell(*load_sol)) {
      if (served > 1e-6) {
        CHECK(served == doctest::Approx(expected_demand_mw).epsilon(1e-3));
        ++load_balanced_blocks;
      }
    }
  }
  CHECK(load_balanced_blocks == expected_balanced_cells);

  // ── 3. Bus balance duals — sim pass with crossover ran ─────────
  // Training forward passes use `crossover=false`, so row duals are
  // never populated.  Only the simulation pass writes finite duals.
  // Without the fix, every dual cell stays NaN regardless of
  // `max_iters` or `memory_mode`.
  const auto bus_dual_root =
      std::filesystem::path(tmpdir) / "Bus" / "balance_dual.parquet";
  REQUIRE(std::filesystem::exists(bus_dual_root));

  std::size_t total_finite_duals = 0;
  for (const auto phase_uid : phase_uids) {
    auto bus_dual = read_parquet_shard(bus_dual_root, scene_uid, phase_uid);
    REQUIRE(bus_dual != nullptr);
    total_finite_duals += count_finite_doubles(*bus_dual);
  }
  // 1 bus × 24 active blocks × 3 phases = 72 finite cells expected.
  // Allow some slack for degenerate-block dual gaps (LP solver may
  // mark a few basic-row duals as null even with crossover); ask
  // for at least half.
  CHECK(total_finite_duals >= expected_balanced_cells / 2);

  // ── 4. solution.csv: 13-column header + row width via the extracted
  //       solution_csv.hpp formatter, exercised on the real write_out() path.
  const auto sol_csv = std::filesystem::path(tmpdir) / "solution.csv";
  REQUIRE(std::filesystem::exists(sol_csv));
  {
    const auto split = [](const std::string& line)
    {
      std::vector<std::string> out;
      std::size_t start = 0;
      for (;;) {
        const auto pos = line.find(',', start);
        out.push_back(line.substr(start, pos - start));
        if (pos == std::string::npos) {
          break;
        }
        start = pos + 1;
      }
      return out;
    };

    std::ifstream in(sol_csv);
    REQUIRE(in.is_open());

    std::string header;
    REQUIRE(std::getline(in, header));
    const auto hcols = split(header);
    REQUIRE(hcols.size() == 13);
    CHECK(hcols[9] == "solve_ticks");
    CHECK(hcols[10] == "solve_time_s");
    CHECK(hcols[11] == "solve_calls");
    CHECK(hcols[12] == "infeasible_count");

    std::string data_row;
    REQUIRE(std::getline(in, data_row));
    const auto rcols = split(data_row);
    REQUIRE(rcols.size() == hcols.size());  // header/row widths stay in sync
    // status (col 2) and status_name (col 3) agree; feasible fixture → optimal.
    if (rcols[2] == "0") {
      CHECK(rcols[3] == "optimal");
    }
    CHECK(std::stod(rcols[10]) >= 0.0);  // solve_time_s parses, non-negative
    CHECK(std::stoul(rcols[12]) == 0);  // infeasible_count: feasible fixture
  }

  std::filesystem::remove_all(tmpdir);
}

}  // namespace

TEST_CASE(  // NOLINT
    "Cascade output: served demand + finite duals "
    "across (memory_mode × max_iterations)")
{
  using namespace gtopt;

  // ── LowMemoryMode::off — backend stays resident ─────────────────
  SUBCASE("off, max_iters=0 (sim pass only)")
  {
    run_cascade_and_check_outputs(LowMemoryMode::off, 0);
  }
  SUBCASE("off, max_iters=1")
  {
    run_cascade_and_check_outputs(LowMemoryMode::off, 1);
  }
  SUBCASE("off, max_iters=5")
  {
    run_cascade_and_check_outputs(LowMemoryMode::off, 5);
  }

  // ── LowMemoryMode::compress — backend released, snapshot path ──
  // This is the mode juan/IPLP uses; the rehydrate-then-write_out
  // round trip is where the 2026-05 sim-pass-skip bug originally
  // surfaced.
  SUBCASE("compress, max_iters=0 (sim pass only)")
  {
    run_cascade_and_check_outputs(LowMemoryMode::compress, 0);
  }
  SUBCASE("compress, max_iters=1")
  {
    run_cascade_and_check_outputs(LowMemoryMode::compress, 1);
  }
  SUBCASE("compress, max_iters=5")
  {
    run_cascade_and_check_outputs(LowMemoryMode::compress, 5);
  }
}
