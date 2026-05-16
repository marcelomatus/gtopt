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
 * `planning_lp.write_out()`, then read the resulting `Generator` and
 * `Bus` parquets back via `parquet::arrow::FileReader` and assert:
 *
 *   1. `generation_sol.parquet` exists at every (scene, phase) cell
 *      and at least one numeric value column carries a finite,
 *      strictly-positive entry — proof that the per-cell solution
 *      vector reached the parquet writer.
 *   2. `balance_dual.parquet` exists and carries finite values
 *      (NOT all-NaN) — proof that the **simulation pass** ran with
 *      `crossover=true`.  Training forward passes set
 *      `crossover=false`, so row duals are unset and the output
 *      file would be all-NaN if the sim pass were skipped.
 *   3. The aggregate generation in MW (sum over generators per block)
 *      matches the demand to within a small tolerance — the network
 *      cannot have unserved load at this scale (50 MW hydro + 500 MW
 *      thermal vs 100 MW demand), so the balance is tight.
 *
 * This test exercises the failure mode caught only by reading the
 * parquet payload, not by checking the in-memory `level_stats` or
 * the `result->size()` — the latter both reported a healthy converged
 * run while the on-disk output was meaningless.
 */

#include <filesystem>
#include <vector>

#include <arrow/io/file.h>
#include <arrow/table.h>
#include <doctest/doctest.h>
#include <gtopt/cascade_method.hpp>
#include <gtopt/json/json_planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <parquet/arrow/reader.h>

#include "cascade_helpers.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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

// Count finite (non-NaN, non-Inf) doubles across every numeric column
// of `table` that isn't a partition / index key.  Used to distinguish
// "sim pass ran" (lots of finite duals/sols) from "sim pass skipped"
// (all-NaN dual columns).
[[nodiscard]] auto count_finite_doubles(const arrow::Table& table)
    -> std::size_t
{
  std::size_t finite_count = 0;
  for (int c = 0; c < table.num_columns(); ++c) {
    const auto& col = table.column(c);
    const auto& field_name = table.schema()->field(c)->name();
    // Skip the partition / index columns (scenario / stage / block,
    // and the hive partition fields scene / phase).  Only count
    // double-typed value columns.
    if (field_name == "scenario" || field_name == "stage"
        || field_name == "block" || field_name == "scene"
        || field_name == "phase")
    {
      continue;
    }
    if (col->type()->id() != arrow::Type::DOUBLE) {
      continue;
    }
    for (int ch = 0; ch < col->num_chunks(); ++ch) {
      const auto chunk =
          std::static_pointer_cast<arrow::DoubleArray>(col->chunk(ch));
      for (int64_t i = 0; i < chunk->length(); ++i) {
        if (chunk->IsNull(i)) {
          continue;
        }
        const auto v = chunk->Value(i);
        if (std::isfinite(v)) {
          ++finite_count;
        }
      }
    }
  }
  return finite_count;
}

// Aggregate `generation_sol` per (stage, block) row: sum across
// every generator column.  Used to check energy balance vs demand.
[[nodiscard]] auto sum_per_row(const arrow::Table& table) -> std::vector<double>
{
  std::vector<double> totals(static_cast<std::size_t>(table.num_rows()), 0.0);
  for (int c = 0; c < table.num_columns(); ++c) {
    const auto& field_name = table.schema()->field(c)->name();
    if (field_name == "scenario" || field_name == "stage"
        || field_name == "block" || field_name == "scene"
        || field_name == "phase")
    {
      continue;
    }
    const auto& col = table.column(c);
    if (col->type()->id() != arrow::Type::DOUBLE) {
      continue;
    }
    int64_t row_offset = 0;
    for (int ch = 0; ch < col->num_chunks(); ++ch) {
      const auto chunk =
          std::static_pointer_cast<arrow::DoubleArray>(col->chunk(ch));
      for (int64_t i = 0; i < chunk->length(); ++i) {
        const auto idx = static_cast<std::size_t>(row_offset + i);
        if (!chunk->IsNull(i)) {
          const auto v = chunk->Value(i);
          if (std::isfinite(v)) {
            totals[idx] += v;
          }
        }
      }
      row_offset += chunk->length();
    }
  }
  return totals;
}

}  // namespace

TEST_CASE(  // NOLINT
    "Cascade: last-level simulation pass writes finite parquet outputs")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  // ── Tmpdir for output ─────────────────────────────────────────
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_cascade_parquet_test";
  std::filesystem::remove_all(tmpdir);
  std::filesystem::create_directories(tmpdir);

  // ── Planning fixture: 3-phase hydro, single bus, single scenario ─
  auto planning = make_3phase_hydro_planning();
  planning.options.output_directory = tmpdir.string();
  planning.options.output_format = DataFormat::parquet;
  // Default fixture asks for csv/uncompressed; override above for
  // parquet round-trip.  `output_compression` stays at uncompressed
  // so the test doesn't depend on snappy / zstd availability.
  PlanningLP planning_lp(std::move(planning));

  // ── Two-level cascade.  L0 short, L1 the rest.  This exercises ──
  // the level-to-level cell transfer AND the last-level sim pass
  // (the failure mode this test guards).
  SDDPOptions base_opts;
  base_opts.max_iterations = 8;
  base_opts.convergence_tol = 0.01;
  base_opts.apertures = std::vector<Uid> {};  // disable apertures
  base_opts.enable_api = false;

  CascadeOptions cascade;
  cascade.level_array = {
      CascadeLevel {
          .name = OptName {"warmup"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {3},
                  .apertures = Array<Uid> {},
              },
      },
      CascadeLevel {
          .name = OptName {"final"},
          .sddp_options =
              CascadeLevelMethod {
                  .max_iterations = OptInt {5},
                  .apertures = Array<Uid> {},
              },
      },
  };

  CascadePlanningMethod solver(std::move(base_opts), std::move(cascade));
  const SolverOptions lp_opts;
  const auto res = solver.solve(planning_lp, lp_opts);
  REQUIRE(res.has_value());

  // Cascade reports two level_stats (one per level).
  REQUIRE(solver.level_stats().size() == 2);

  // ── Write parquet outputs to tmpdir ───────────────────────────
  planning_lp.write_out();

  // OutputContext partitions on the SCENE uid (0-indexed; SceneLP
  // is built from scenario_array grouping), not the scenario uid.
  // The 3-phase hydro fixture has one scenario folded into scene 0.
  // Phase UIDs are 1 / 2 / 3 (as declared in phase_array).
  constexpr gtopt::uid_t scene_uid = 0;
  constexpr std::array<gtopt::uid_t, 3> phase_uids = {1, 2, 3};

  // ── 1. Generator/generation_sol — primal solutions ────────────
  // Existence + finite values across every phase shard.
  const auto gen_sol_root =
      std::filesystem::path(tmpdir) / "Generator" / "generation_sol.parquet";
  REQUIRE(std::filesystem::exists(gen_sol_root));

  // OutputContext emits 72 rows per shard (one per simulation block);
  // only 24 belong to the active phase, the rest are NaN-filled.
  for (const auto phase_uid : phase_uids) {
    CAPTURE(phase_uid);
    auto gen_sol = read_parquet_shard(gen_sol_root, scene_uid, phase_uid);
    REQUIRE(gen_sol != nullptr);
    CHECK(gen_sol->num_rows() == 72);
    const auto finite_gen = count_finite_doubles(*gen_sol);
    // 24 active blocks × 2 generators = 48 finite cells per phase.
    CHECK(finite_gen >= 24);  // every active block has ≥1 generator dispatched
  }

  // ── 2. Bus/balance_dual — proves the simulation pass ran ──────
  // Training forward passes run with crossover=false, so row duals
  // are NEVER populated.  Only the simulation pass (which uses
  // crossover=true in run_scene_simulation) writes finite duals.
  // If this assertion ever fails, the sim pass was silently
  // skipped — exactly the bug the 2026-05 `should_stop()` patch
  // in `scene_finished` fixed.
  const auto bus_dual_root =
      std::filesystem::path(tmpdir) / "Bus" / "balance_dual.parquet";
  REQUIRE(std::filesystem::exists(bus_dual_root));

  std::size_t total_finite_duals = 0;
  for (const auto phase_uid : phase_uids) {
    auto bus_dual = read_parquet_shard(bus_dual_root, scene_uid, phase_uid);
    REQUIRE(bus_dual != nullptr);
    total_finite_duals += count_finite_doubles(*bus_dual);
  }
  // 1 bus × 24 active blocks × 3 phases = 72 dual cells.  The
  // remaining 24×3 = 72 inactive blocks per shard stay NaN.
  // Tolerate a few degenerate-block dual gaps (LP solver may
  // mark some basic-row duals as null) by asking for at least
  // half of the expected finite count.
  CHECK(total_finite_duals >= 36);

  // ── 3. Energy balance: Σ generation ≈ demand per block ────────
  // Demand is 100 MW.  Hydro+thermal capacity (50 + 500 = 550 MW)
  // is far above demand, so the LP serves it fully.  Sum
  // generation across the two generators per block and compare.
  std::size_t balanced_blocks = 0;
  for (const auto phase_uid : phase_uids) {
    CAPTURE(phase_uid);
    auto gen_sol = read_parquet_shard(gen_sol_root, scene_uid, phase_uid);
    REQUIRE(gen_sol != nullptr);
    const auto per_block_gen = sum_per_row(*gen_sol);
    REQUIRE(per_block_gen.size() == 72);
    for (std::size_t b = 0; b < per_block_gen.size(); ++b) {
      // Inactive blocks for this phase are NaN-filled → sum_per_row
      // skips them and leaves the slot at 0.0.  Only check the
      // active blocks where generation was actually written.
      if (per_block_gen[b] > 1e-6) {
        CAPTURE(b);
        CHECK(per_block_gen[b] == doctest::Approx(100.0).epsilon(1e-3));
        ++balanced_blocks;
      }
    }
  }
  // 3 phases × 24 active blocks = 72 balanced (demand, hydro+thermal)
  // entries.  Any drop below this means the sim pass under-wrote the
  // shard — the original "sim skipped" bug surfaces as 0 here.
  CHECK(balanced_blocks == 72);

  // ── Cleanup ───────────────────────────────────────────────────
  std::filesystem::remove_all(tmpdir);
}
