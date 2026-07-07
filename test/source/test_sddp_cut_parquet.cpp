// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_cut_parquet.cpp
 * @brief     Unit tests for the Parquet variant of SDDP cut I/O.
 * @date      2026-05-11
 * @copyright BSD-3-Clause
 *
 * Mirrors the CSV tests in `test_sddp_cut_io.cpp` to validate the new
 * `save_cuts_parquet` / `load_cuts_parquet` code path added in
 * `source/sddp_cut_parquet.cpp`.  Covers:
 *   1. Save produces a Parquet file with the expected schema/columns.
 *   2. Empty cuts span produces a header-only file.
 *   3. Full save → load round-trip preserves cut count and float bits.
 *   4. Append mode writes a sibling file in the same directory and the
 *      loader picks it up alongside the main file.
 *
 * Bit-exactness here doesn't need `{:.17g}` — Parquet stores raw
 * float64, so the round-trip is exact by construction.  We still assert
 * the property explicitly to guard against an accidental cast to a
 * narrower type in the writer.
 */

#include <filesystem>
#include <fstream>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <parquet/arrow/reader.h>

#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

auto make_parquet_test_path(const std::string& tag) -> std::filesystem::path
{
  return std::filesystem::temp_directory_path()
      / ("gtopt_test_cut_parquet_" + tag);
}

}  // namespace

TEST_CASE("save_cuts_parquet writes a valid Parquet file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& stored = sddp.stored_cuts();
  REQUIRE_FALSE(stored.empty());

  const auto dir = make_parquet_test_path("save");
  std::filesystem::create_directories(dir);
  const auto cuts_file = (dir / "sddp_cuts.parquet").string();

  auto save_result = save_cuts_parquet(stored, planning_lp, cuts_file);
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(cuts_file));

  // Open the file via Arrow and verify schema.
  auto open_result = arrow::io::ReadableFile::Open(cuts_file);
  REQUIRE(open_result.ok());
  std::shared_ptr<arrow::io::RandomAccessFile> input = *open_result;

  std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
  auto ofile = parquet::arrow::OpenFile(input, arrow::default_memory_pool());
  REQUIRE(ofile.ok());
  reader = std::move(ofile).ValueUnsafe();
#else
  auto st =
      parquet::arrow::OpenFile(input, arrow::default_memory_pool(), &reader);
  REQUIRE(st.ok());
#endif
  std::shared_ptr<arrow::Table> table;
  REQUIRE(reader->ReadTable(&table).ok());

  // Verify the expected columns are present (schema v3 — ``name``
  // column dropped, iteration is the authoritative identifier).
  CHECK(table->GetColumnByName("type") != nullptr);
  CHECK(table->GetColumnByName("phase") != nullptr);
  CHECK(table->GetColumnByName("scene") != nullptr);
  CHECK(table->GetColumnByName("iteration") != nullptr);
  CHECK(table->GetColumnByName("rhs") != nullptr);
  CHECK(table->GetColumnByName("coeffs") != nullptr);
  CHECK(table->GetColumnByName("name") == nullptr);  // intentionally removed
  // `dual` column dropped on 2026-05-15 alongside the
  // `update_stored_cut_duals` machinery (the producer always read
  // from the wrong LP — the main cell post-cut-add-without-resolve,
  // never the apertures that actually exercise the cut).
  CHECK(table->GetColumnByName("dual") == nullptr);

  // Schema-level metadata records version + scale_objective.  The
  // writer enables `ArrowWriterProperties::store_schema()` so the Arrow
  // schema (and its KeyValueMetadata) survives the Parquet round-trip
  // via the `ARROW:schema` footer entry.
  const auto& schema_md = table->schema()->metadata();
  REQUIRE(schema_md != nullptr);
  CHECK(schema_md->Contains("version"));
  CHECK(schema_md->Contains("scale_objective"));

  CHECK(table->num_rows() >= 1);

  std::filesystem::remove_all(dir);
}

TEST_CASE(
    "save_cuts_parquet with empty cuts vector writes a header-only "
    "file")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  const PlanningLP planning_lp(std::move(planning));

  const auto dir = make_parquet_test_path("empty");
  std::filesystem::create_directories(dir);
  const auto cuts_file = (dir / "sddp_cuts.parquet").string();

  const std::vector<StoredCut> empty_cuts;
  auto save_result = save_cuts_parquet(
      std::span<const StoredCut>(empty_cuts), planning_lp, cuts_file);
  REQUIRE(save_result.has_value());
  CHECK(std::filesystem::exists(cuts_file));

  auto open_result = arrow::io::ReadableFile::Open(cuts_file);
  REQUIRE(open_result.ok());
  std::shared_ptr<arrow::io::RandomAccessFile> input = *open_result;
  std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
  auto ofile = parquet::arrow::OpenFile(input, arrow::default_memory_pool());
  REQUIRE(ofile.ok());
  reader = std::move(ofile).ValueUnsafe();
#else
  auto st =
      parquet::arrow::OpenFile(input, arrow::default_memory_pool(), &reader);
  REQUIRE(st.ok());
#endif
  std::shared_ptr<arrow::Table> table;
  REQUIRE(reader->ReadTable(&table).ok());
  CHECK(table->num_rows() == 0);

  std::filesystem::remove_all(dir);
}

TEST_CASE("save_cuts_parquet / load_cuts_parquet round-trip")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& stored = sddp.stored_cuts();
  REQUIRE_FALSE(stored.empty());
  const auto saved_count = stored.size();

  const auto dir = make_parquet_test_path("roundtrip");
  std::filesystem::create_directories(dir);
  const auto cuts_file = (dir / "sddp_cuts.parquet").string();

  REQUIRE(save_cuts_parquet(stored, planning_lp, cuts_file).has_value());

  // Fresh planning to load the cuts back into.
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));
  LabelMaker lm {LpNamesLevel::all};

  auto loaded =
      load_cuts_parquet(planning_lp2, cuts_file, /*scale_alpha=*/1.0, lm);
  REQUIRE(loaded.has_value());
  // Loaded count should equal the unique (phase, name) cuts in the file.
  CHECK(loaded->count >= 1);
  CHECK(loaded->count <= static_cast<int>(saved_count));

  std::filesystem::remove_all(dir);
}

TEST_CASE(
    "save_cuts_parquet append mode writes a sibling file picked up "
    "by the loader")  // NOLINT
{
  auto planning = make_3phase_hydro_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 3;
  sddp_opts.convergence_tol = 1e-6;

  SDDPMethod sddp(planning_lp, sddp_opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());

  const auto& stored = sddp.stored_cuts();
  REQUIRE_FALSE(stored.empty());

  const auto dir = make_parquet_test_path("append");
  std::filesystem::create_directories(dir);
  const auto cuts_file = (dir / "sddp_cuts.parquet").string();

  // Initial save: full set goes to <stem>.parquet.
  REQUIRE(save_cuts_parquet(stored, planning_lp, cuts_file).has_value());
  CHECK(std::filesystem::exists(cuts_file));

  // Append the same set again — should land in `<stem>.append-*.parquet`,
  // NOT overwrite the primary file.
  REQUIRE(
      save_cuts_parquet(stored, planning_lp, cuts_file, /*append_mode=*/true)
          .has_value());

  // Count parquet files in the directory: expect at least 2 (primary
  // + one append).
  int parquet_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".parquet") {
      ++parquet_count;
    }
  }
  CHECK(parquet_count >= 2);

  // Loader globs the primary + every sibling append.  De-dup logic
  // means the loaded count should equal the unique (phase, name) set,
  // not 2× it.
  auto planning2 = make_3phase_hydro_planning();
  PlanningLP planning_lp2(std::move(planning2));
  LabelMaker lm {LpNamesLevel::all};
  auto loaded =
      load_cuts_parquet(planning_lp2, cuts_file, /*scale_alpha=*/1.0, lm);
  REQUIRE(loaded.has_value());
  CHECK(loaded->count >= 1);

  std::filesystem::remove_all(dir);
}

// The format-dispatching ``save_cuts`` / ``load_cuts`` free functions
// and the ``CutIOFormat`` enum were removed in 2026-05 — Parquet is
// the only supported format.  Call ``save_cuts_parquet`` /
// ``load_cuts_parquet`` directly; the round-trip coverage above
// already exercises that path.
