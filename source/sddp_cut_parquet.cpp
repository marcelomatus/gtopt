/**
 * @file      sddp_cut_parquet.cpp
 * @brief     Apache Parquet save / load for SDDP cuts.
 * @date      2026-05-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The single canonical on-disk format for SDDP cuts.  Uses a typed
 * Arrow schema with a `list<struct<key,val>>` column for cut
 * coefficients — eliminating the variable-schema CSV tail and the
 * `{:.17g}` text-formatting dance.  Bit-exactness comes free from
 * float64 storage in Parquet.  The legacy CSV and JSON cut writers
 * were retired in 2026-05.
 *
 * On-disk schema (v3, 2026-05):
 *     type:      int8        (CutType enum's underlying uint8_t)
 *     phase:     int32
 *     scene:     int32
 *     iteration: int32       (direct, no longer extracted from a name field)
 *     extra:     int32       (4th element of IterationContext — multi-cut
 *                             sibling discriminator, sentinel -1 = unset)
 *     rhs:       float64
 *     dual:      float64?    (nullable)
 *     coeffs:    list<struct<key: utf8, val: float64>>
 *
 * The legacy schema-v2 ``name: utf8`` column was dropped in 2026-05; cut
 * identity now lives in the structured ``CutKey {type, scene_uid,
 * phase_uid, iteration_index, extra}`` 5-tuple.  LP row labels are
 * still generated at install time by ``LabelMaker::make_row_label``
 * from the SparseRow metadata (class_name / constraint_name / uid /
 * context), but those are an LP-display concern and never serialised
 * back to the cut file.
 *
 * File metadata: KeyValueMetadata{version = "3", scale_objective = "<.17g>"}.
 *
 * Append mode: Parquet has no row-level append, so `save_cuts_parquet(...,
 * append_mode=true)` writes a sibling file `<stem>.append-<stamp>.parquet`
 * in the same directory.  `load_cuts_parquet` globs `<stem>.parquet` and
 * every `<stem>.append-*.parquet` and merges them.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <gtopt/fmap.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_io_internal.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{
using namespace gtopt::detail;

namespace
{

auto make_io_error(const std::string& filepath, const std::string& detail)
    -> Error
{
  return Error {
      .code = ErrorCode::FileIOError,
      .message = std::format("{}: {}", filepath, detail),
  };
}

/// Build the Arrow schema for an SDDP cut file (with `scale_objective`
/// recorded in the file-level key-value metadata).
auto make_cuts_schema(double scale_objective) -> std::shared_ptr<arrow::Schema>
{
  auto coeff_struct_type = arrow::struct_({
      arrow::field("key", arrow::utf8(), /*nullable=*/false),
      arrow::field("val", arrow::float64(), /*nullable=*/false),
  });
  auto kv = arrow::KeyValueMetadata::Make(
      {"version", "scale_objective"},
      {"2", std::format("{:.17g}", scale_objective)});
  // Schema v3 (post-2026-05):
  //  * Drop the legacy ``name`` column — the 4-tuple
  //    ``(scene, phase, iteration, type)`` uniquely identifies every
  //    cut by construction (each SDDP iter's backward pass adds at
  //    most one optimality + one feasibility cut per
  //    ``(scene, phase)`` cell), so the human-readable name is
  //    redundant metadata for parquet.  Legacy ``name`` column is
  //    silently ignored on load.
  //  * ``type`` is the ``CutType`` enum's underlying ``uint8_t`` value
  //    (was previously a 1-char string "f"/"o"); see
  //    :enum:`gtopt::CutType` in
  //    ``include/gtopt/sddp_cut_store_enums.hpp``.  Legacy files
  //    written with the string form are tolerated on load via a
  //    runtime type check on the column.
  return arrow::schema(
      {
          arrow::field("type", arrow::int8(), /*nullable=*/false),
          arrow::field("phase", arrow::int32(), /*nullable=*/false),
          arrow::field("scene", arrow::int32(), /*nullable=*/false),
          arrow::field("iteration", arrow::int32(), /*nullable=*/false),
          // ``extra`` is :member:`StoredCut::extra` — the 4th element
          // of the source ``IterationContext``.  Required to keep
          // multi-cut feasibility siblings distinguishable on
          // save/load: forward-pass ``multi_cut`` mode emits multiple
          // fcuts per ``(scene, phase, iter)`` cell, all sharing
          // ``type=Feasibility``; the ``extra`` discriminator is
          // what makes the :class:`CutKey` 5-tuple unique.
          arrow::field("extra", arrow::int32(), /*nullable=*/false),
          arrow::field("rhs", arrow::float64(), /*nullable=*/false),
          arrow::field("dual", arrow::float64(), /*nullable=*/true),
          arrow::field("coeffs",
                       arrow::list(coeff_struct_type),
                       /*nullable=*/false),
      },
      kv);
}

/// Build an Arrow Table from a span of StoredCut + a phase→ColKeyMap.
/// Returns the table or an Error.  The caller passes the precomputed
/// scale_objective so it can be stamped into the schema metadata.
auto build_cuts_table(
    std::span<const StoredCut> cuts,
    const flat_map<PhaseUid, PhaseIndex>& phase_map,
    const std::unordered_map<PhaseIndex, ColKeyMap>& phase_col_keys,
    double scale_objective,
    const std::string& filepath_for_error)
    -> std::expected<std::shared_ptr<arrow::Table>, Error>
{
  auto* pool = arrow::default_memory_pool();

  arrow::Int8Builder type_b {pool};
  arrow::Int32Builder phase_b {pool};
  arrow::Int32Builder scene_b {pool};
  arrow::Int32Builder iter_b {pool};
  arrow::Int32Builder extra_b {pool};
  arrow::DoubleBuilder rhs_b {pool};
  arrow::DoubleBuilder dual_b {pool};

  // List<Struct<key, val>> for coefficients
  auto coeff_struct_type = arrow::struct_({
      arrow::field("key", arrow::utf8(), false),
      arrow::field("val", arrow::float64(), false),
  });
  auto coeff_key_b = std::make_shared<arrow::StringBuilder>(pool);
  auto coeff_val_b = std::make_shared<arrow::DoubleBuilder>(pool);
  auto coeff_struct_b = std::make_shared<arrow::StructBuilder>(
      coeff_struct_type,
      pool,
      std::vector<std::shared_ptr<arrow::ArrayBuilder>> {
          coeff_key_b,
          coeff_val_b,
      });
  arrow::ListBuilder coeff_list_b {pool, coeff_struct_b};

  const auto reserve_status =
      [&](const arrow::Status& s) -> std::expected<void, Error>
  {
    if (!s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    return {};
  };
  if (auto e = reserve_status(type_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(phase_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(scene_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(iter_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(extra_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(rhs_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(dual_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }

  for (const auto& cut : cuts) {
    // Store the CutType enum directly as its underlying uint8_t
    // (matches the ``int8`` schema field); see
    // :enum:`gtopt::CutType`.
    if (auto s = type_b.Append(
            static_cast<int8_t>(static_cast<std::uint8_t>(cut.type)));
        !s.ok())
    {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = phase_b.Append(static_cast<int32_t>(cut.phase_uid)); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = scene_b.Append(static_cast<int32_t>(cut.scene_uid)); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s =
            iter_b.Append(static_cast<int32_t>(uid_of(cut.iteration_index)));
        !s.ok())
    {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = extra_b.Append(cut.extra); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = rhs_b.Append(cut.rhs); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (cut.dual.has_value()) {
      if (auto s = dual_b.Append(*cut.dual); !s.ok()) {
        return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
      }
    } else {
      if (auto s = dual_b.AppendNull(); !s.ok()) {
        return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
      }
    }

    // Start a new list element for this cut's coefficient vector.
    if (auto s = coeff_list_b.Append(); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    auto pit = phase_map.find(cut.phase_uid);
    if (pit == phase_map.end()) {
      // Cut targets an unknown phase — write a non-resolving placeholder
      // for each coefficient so the file round-trips its structure but
      // the loader can surface the issue.  Same fallback as
      // `write_cut_coefficients_unscaled` in the CSV path.
      for (const auto& [col, coeff] : cut.coefficients) {
        if (auto s = coeff_struct_b->Append(); !s.ok()) {
          return std::unexpected(
              make_io_error(filepath_for_error, s.ToString()));
        }
        if (auto s = coeff_key_b->Append(std::format("{}", col)); !s.ok()) {
          return std::unexpected(
              make_io_error(filepath_for_error, s.ToString()));
        }
        if (auto s = coeff_val_b->Append(coeff); !s.ok()) {
          return std::unexpected(
              make_io_error(filepath_for_error, s.ToString()));
        }
      }
      continue;
    }
    const auto& col_keys = phase_col_keys.at(pit->second);
    for (const auto& [col, coeff] : cut.coefficients) {
      const auto it = col_keys.find(col);
      if (it == col_keys.end()) {
        return std::unexpected(make_io_error(
            filepath_for_error,
            std::format("cut (phase={}, scene={}) references col {} that "
                        "is not a registered state variable",
                        cut.phase_uid,
                        cut.scene_uid,
                        col)));
      }
      const auto& [cls, var, uid] = it->second;
      if (auto s = coeff_struct_b->Append(); !s.ok()) {
        return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
      }
      if (auto s = coeff_key_b->Append(std::format("{}:{}:{}", cls, var, uid));
          !s.ok())
      {
        return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
      }
      if (auto s = coeff_val_b->Append(coeff); !s.ok()) {
        return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
      }
    }
  }

  std::shared_ptr<arrow::Array> type_a;
  std::shared_ptr<arrow::Array> phase_a;
  std::shared_ptr<arrow::Array> scene_a;
  std::shared_ptr<arrow::Array> iter_a;
  std::shared_ptr<arrow::Array> extra_a;
  std::shared_ptr<arrow::Array> rhs_a;
  std::shared_ptr<arrow::Array> dual_a;
  std::shared_ptr<arrow::Array> coeffs_a;
  if (auto s = type_b.Finish(&type_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = phase_b.Finish(&phase_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = scene_b.Finish(&scene_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = iter_b.Finish(&iter_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = extra_b.Finish(&extra_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = rhs_b.Finish(&rhs_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = dual_b.Finish(&dual_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = coeff_list_b.Finish(&coeffs_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }

  auto schema = make_cuts_schema(scale_objective);
  return arrow::Table::Make(
      std::move(schema),
      {type_a, phase_a, scene_a, iter_a, extra_a, rhs_a, dual_a, coeffs_a});
}

/// Resolve the actual output path for a save call.  In append mode the
/// caller-supplied `filepath` is rewritten to a sibling with a unique
/// suffix so multiple appends never clobber each other.
auto resolve_save_path(const std::string& filepath, bool append_mode)
    -> std::string
{
  if (!append_mode) {
    return filepath;
  }
  const auto p = std::filesystem::path(filepath);
  const auto stamp =
      std::chrono::system_clock::now().time_since_epoch().count();
  const auto new_name =
      std::format("{}.append-{}.parquet", p.stem().string(), stamp);
  return (p.parent_path() / new_name).string();
}

}  // namespace

[[nodiscard]] auto save_cuts_parquet(std::span<const StoredCut> cuts,
                                     const PlanningLP& planning_lp,
                                     const std::string& filepath,
                                     bool append_mode)
    -> std::expected<void, Error>
{
  try {
    const auto parent = std::filesystem::path(filepath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    const auto& rep_li =
        planning_lp.system(first_scene_index(), first_phase_index())
            .linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    const auto phase_map = build_phase_uid_map(planning_lp);
    const auto& sim = planning_lp.simulation();
    std::unordered_map<PhaseIndex, ColKeyMap> phase_col_keys;
    map_reserve(phase_col_keys, phase_map.size());
    for (const auto& [uid, pi] : phase_map) {
      phase_col_keys.try_emplace(
          pi, build_col_key_map(sim, first_scene_index(), pi));
    }

    auto table_result =
        build_cuts_table(cuts, phase_map, phase_col_keys, scale_obj, filepath);
    if (!table_result.has_value()) {
      return std::unexpected(table_result.error());
    }
    const auto& table = *table_result;

    const auto out_path = resolve_save_path(filepath, append_mode);

    auto open_result = arrow::io::FileOutputStream::Open(out_path);
    if (!open_result.ok()) {
      return std::unexpected(
          make_io_error(out_path, open_result.status().ToString()));
    }
    const auto& out = *open_result;

    parquet::WriterProperties::Builder props_builder;
    props_builder.compression(parquet::Compression::SNAPPY);
    const auto props = props_builder.build();

    // `store_schema()` writes the Arrow schema (including its
    // KeyValueMetadata) into the Parquet file footer as
    // ``ARROW:schema``, so `version` / `scale_objective` survive the
    // write/read round-trip.  Without this flag, parquet::arrow
    // discards Arrow schema metadata on write.
    parquet::ArrowWriterProperties::Builder arrow_props_builder;
    arrow_props_builder.store_schema();
    const auto arrow_props = arrow_props_builder.build();

    if (auto s = parquet::arrow::WriteTable(*table,
                                            arrow::default_memory_pool(),
                                            out,
                                            /*chunk_size=*/1024UL * 1024UL,
                                            props,
                                            arrow_props);
        !s.ok())
    {
      return std::unexpected(make_io_error(out_path, s.ToString()));
    }
    if (auto s = out->Close(); !s.ok()) {
      return std::unexpected(make_io_error(out_path, s.ToString()));
    }

    SPDLOG_TRACE("SDDP: saved {} cuts to {}", cuts.size(), out_path);
    return {};
  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error saving cuts to {}: {}", filepath, e.what()),
    });
  }
}

[[nodiscard]] auto save_scene_cuts_parquet(std::span<const StoredCut> cuts,
                                           SceneIndex scene_index,
                                           SceneUid scene_uid,
                                           const PlanningLP& planning_lp,
                                           const std::string& directory)
    -> std::expected<void, Error>
{
  try {
    std::filesystem::create_directories(directory);

    const auto filepath = (std::filesystem::path(directory)
                           / std::format(sddp_file::scene_cuts_fmt, scene_uid))
                              .string();

    const auto& rep_li =
        planning_lp.system(scene_index, first_phase_index()).linear_interface();
    const auto scale_obj = rep_li.scale_objective();

    const auto phase_map = build_phase_uid_map(planning_lp);
    const auto& sim = planning_lp.simulation();
    std::unordered_map<PhaseIndex, ColKeyMap> phase_col_keys;
    map_reserve(phase_col_keys, phase_map.size());
    for (const auto& [uid, pi] : phase_map) {
      phase_col_keys.try_emplace(pi, build_col_key_map(sim, scene_index, pi));
    }

    auto table_result =
        build_cuts_table(cuts, phase_map, phase_col_keys, scale_obj, filepath);
    if (!table_result.has_value()) {
      return std::unexpected(table_result.error());
    }
    const auto& table = *table_result;

    auto open_result = arrow::io::FileOutputStream::Open(filepath);
    if (!open_result.ok()) {
      return std::unexpected(
          make_io_error(filepath, open_result.status().ToString()));
    }
    const auto& out = *open_result;

    parquet::WriterProperties::Builder props_builder;
    props_builder.compression(parquet::Compression::SNAPPY);
    const auto props = props_builder.build();
    if (auto s = parquet::arrow::WriteTable(*table,
                                            arrow::default_memory_pool(),
                                            out,
                                            /*chunk_size=*/1024UL * 1024UL,
                                            props);
        !s.ok())
    {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }
    if (auto s = out->Close(); !s.ok()) {
      return std::unexpected(make_io_error(filepath, s.ToString()));
    }

    SPDLOG_TRACE("SDDP: saved {} cuts for scene UID {} to {}",
                 cuts.size(),
                 scene_uid,
                 filepath);
    return {};
  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format("Error saving scene UID {} cuts to {}: {}",
                               scene_uid,
                               directory,
                               e.what()),
    });
  }
}

// ─── Load ─────────────────────────────────────────────────────────────────

namespace
{

/// Open one Parquet file and return the materialised arrow::Table.
auto read_parquet_table(const std::string& filepath)
    -> std::expected<std::shared_ptr<arrow::Table>, Error>
{
  auto open_result = arrow::io::ReadableFile::Open(filepath);
  if (!open_result.ok()) {
    return std::unexpected(
        make_io_error(filepath, open_result.status().ToString()));
  }
  const std::shared_ptr<arrow::io::RandomAccessFile> input = *open_result;

  std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
  auto ofile = parquet::arrow::OpenFile(input, arrow::default_memory_pool());
  if (!ofile.ok()) {
    return std::unexpected(make_io_error(filepath, ofile.status().ToString()));
  }
  reader = std::move(ofile).ValueUnsafe();
#else
  auto st =
      parquet::arrow::OpenFile(input, arrow::default_memory_pool(), &reader);
  if (!st.ok()) {
    return std::unexpected(make_io_error(filepath, st.ToString()));
  }
#endif

  std::shared_ptr<arrow::Table> table;
  if (auto s = reader->ReadTable(&table); !s.ok()) {
    return std::unexpected(make_io_error(filepath, s.ToString()));
  }
  return table;
}

/// Collect the main `<stem>.parquet` file plus every sibling
/// `<stem>.append-*.parquet` file in the same directory (sorted by name
/// for deterministic load order).
auto collect_parquet_files(const std::string& filepath)
    -> std::vector<std::string>
{
  const auto p = std::filesystem::path(filepath);
  const auto stem = p.stem().string();
  const auto dir = p.parent_path();

  std::vector<std::string> files;
  if (std::filesystem::exists(filepath)) {
    files.push_back(filepath);
  }
  if (std::filesystem::exists(dir)) {
    const auto prefix = stem + ".append-";
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto name = entry.path().filename().string();
      if (name.starts_with(prefix) && name.ends_with(".parquet")) {
        files.push_back(entry.path().string());
      }
    }
  }
  std::ranges::sort(files);
  return files;
}

/// Add one cut row to the per-cell accumulator (matches the CSV loader's
/// pass-1 staging).
struct CellKey
{
  SceneIndex scene;
  PhaseIndex phase;
  friend constexpr auto operator<=>(const CellKey&, const CellKey&) = default;
};
struct CellCuts
{
  std::vector<SparseRow> opt;
  std::vector<SparseRow> feas;
};

}  // namespace

[[nodiscard]] auto load_cuts_parquet(
    PlanningLP& planning_lp,
    const std::string& filepath,
    [[maybe_unused]] double scale_alpha,
    [[maybe_unused]] const LabelMaker& label_maker,
    [[maybe_unused]] const StrongIndexVector<
        SceneIndex,
        StrongIndexVector<PhaseIndex, PhaseStateInfo>>* scene_phase_states,
    SDDPCutManager* cut_store) -> std::expected<CutLoadResult, Error>
{
  try {
    const auto files = collect_parquet_files(filepath);
    if (files.empty()) {
      return std::unexpected(make_io_error(
          filepath,
          "no Parquet cut files found (looked for <stem>.parquet and "
          "sibling <stem>.append-*.parquet)"));
    }

    CutLoadResult result {};
    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();
    const auto phase_uid_to_index = build_phase_uid_map(planning_lp);

    // Per (scene, phase, type) accumulator — mirrors the CSV loader.
    flat_map<CellKey, CellCuts> accum;
    map_reserve(accum,
                static_cast<size_t>(num_scenes) * phase_uid_to_index.size());

    // De-dup across the full :class:`CutKey` 5-tuple — the same cut
    // may appear in multiple files (combined + append-deltas).  All
    // five components matter for uniqueness: ``scene_uid`` (so two
    // distinct cuts at the same (phase, iter) on different scenes
    // are kept separate), ``phase_uid``, ``iteration_index``,
    // ``type``, and ``extra`` (so multi-cut feasibility siblings
    // emitted by forward-pass ``multi_cut`` mode at the same
    // ``(scene, phase, iter, type=Feasibility)`` are not collapsed
    // into one on load).
    std::set<CutKey> loaded_keys;

    for (const auto& fname : files) {
      auto table_result = read_parquet_table(fname);
      if (!table_result.has_value()) {
        return std::unexpected(table_result.error());
      }
      auto& table = *table_result;

      auto type_col = table->GetColumnByName("type");
      auto phase_col = table->GetColumnByName("phase");
      auto scene_col = table->GetColumnByName("scene");
      auto iter_col = table->GetColumnByName("iteration");
      auto extra_col = table->GetColumnByName("extra");
      auto rhs_col = table->GetColumnByName("rhs");
      auto coeffs_col = table->GetColumnByName("coeffs");
      if (!type_col || !phase_col || !rhs_col || !coeffs_col) {
        return std::unexpected(make_io_error(
            fname, "missing one of required columns (type/phase/rhs/coeffs)"));
      }
      // ``scene`` and ``iteration`` are optional only for back-compat
      // with legacy parquet files that pre-date the schema v3 update.
      // For schema v3+ files (post-2026-05) both columns are present;
      // ``scene`` is required for per-scene routing (otherwise we
      // broadcast, multiplying cut counts by N_scenes per level) and
      // ``iteration`` replaces the dropped ``name`` column.  Legacy
      // ``name`` column (when present) is silently ignored.

      for (int chunk_i = 0; chunk_i < type_col->num_chunks(); ++chunk_i) {
        // Schema v3 stores ``type`` as int8 (CutType enum's underlying
        // uint8_t); legacy files used a 1-char utf8 string ("f"/"o").
        // Detect both at runtime so old parquet files still load.
        auto type_int_arr = std::dynamic_pointer_cast<arrow::Int8Array>(
            type_col->chunk(chunk_i));
        std::shared_ptr<arrow::StringArray> type_str_arr;
        if (!type_int_arr) {
          type_str_arr = std::dynamic_pointer_cast<arrow::StringArray>(
              type_col->chunk(chunk_i));
        }
        auto phase_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
            phase_col->chunk(chunk_i));
        std::shared_ptr<arrow::Int32Array> scene_arr;
        if (scene_col) {
          scene_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
              scene_col->chunk(chunk_i));
        }
        std::shared_ptr<arrow::Int32Array> iter_arr;
        if (iter_col) {
          iter_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
              iter_col->chunk(chunk_i));
        }
        std::shared_ptr<arrow::Int32Array> extra_arr;
        if (extra_col) {
          extra_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
              extra_col->chunk(chunk_i));
        }
        auto rhs_arr = std::dynamic_pointer_cast<arrow::DoubleArray>(
            rhs_col->chunk(chunk_i));
        auto coeffs_arr = std::dynamic_pointer_cast<arrow::ListArray>(
            coeffs_col->chunk(chunk_i));
        if ((!type_int_arr && !type_str_arr) || !phase_arr || !rhs_arr
            || !coeffs_arr)
        {
          return std::unexpected(make_io_error(
              fname, "unexpected column types — schema mismatch"));
        }
        auto coeffs_struct =
            std::dynamic_pointer_cast<arrow::StructArray>(coeffs_arr->values());
        if (!coeffs_struct) {
          return std::unexpected(
              make_io_error(fname, "coeffs column is not list<struct>"));
        }
        auto coeff_key_arr = std::dynamic_pointer_cast<arrow::StringArray>(
            coeffs_struct->field(0));
        auto coeff_val_arr = std::dynamic_pointer_cast<arrow::DoubleArray>(
            coeffs_struct->field(1));
        if (!coeff_key_arr || !coeff_val_arr) {
          return std::unexpected(
              make_io_error(fname, "coeffs struct missing key/val fields"));
        }

        const auto nrows =
            type_int_arr ? type_int_arr->length() : type_str_arr->length();
        for (int64_t i = 0; i < nrows; ++i) {
          // Schema v3 (int8): direct cast from CutType's underlying
          // uint8_t.  Legacy schema (utf8): map "f" → Feasibility,
          // anything else → Optimality.
          const CutType cut_type = type_int_arr
              ? static_cast<CutType>(
                    static_cast<std::uint8_t>(type_int_arr->Value(i)))
              : ((type_str_arr->GetView(i) == "f") ? CutType::Feasibility
                                                   : CutType::Optimality);
          const auto phase_uid = make_uid<Phase>(phase_arr->Value(i));
          const auto rhs = rhs_arr->Value(i);

          auto pit = phase_uid_to_index.find(phase_uid);
          if (pit == phase_uid_to_index.end()) {
            SPDLOG_WARN(
                "SDDP load_cuts_parquet: unknown phase UID {} in {} at row "
                "{}, skipping",
                phase_uid,
                fname,
                i);
            continue;
          }
          const auto phase_index = pit->second;

          // Read scene_uid (sentinel 0 if column absent → legacy file).
          const auto row_scene_uid =
              scene_arr ? make_uid<Scene>(scene_arr->Value(i)) : SceneUid {};
          // Read iteration_index directly from the ``iteration`` column
          // (schema v3+).  Legacy parquet files that pre-date the
          // schema update don't have this column and report iter 0 —
          // the caller's ``max_iteration`` tracker would then under-
          // count for hot-start offset, but the routing + dedup still
          // works correctly.
          const auto cut_iter_idx = iter_arr
              ? IterationIndex {iter_arr->Value(i)}
              : IterationIndex {};
          // ``extra`` defaults to 0 for legacy files (schema v3+
          // emits this column; older files don't).  Zero is a safe
          // default for single-cut paths; multi-cut feasibility cuts
          // would have unique non-zero ``extra`` values when written
          // by a current emitter.
          const auto cut_extra = extra_arr ? extra_arr->Value(i) : 0;
          const auto row_key = CutKey {.type = cut_type,
                                       .scene_uid = row_scene_uid,
                                       .phase_uid = phase_uid,
                                       .iteration_index = cut_iter_idx,
                                       .extra = cut_extra};
          if (!loaded_keys.insert(row_key).second) {
            continue;  // already seen (combined + append duplicate)
          }

          result.max_iteration = std::max(result.max_iteration, cut_iter_idx);

          // Resolve coefficient list for this row.
          const auto list_start = coeffs_arr->value_offset(i);
          const auto list_end = coeffs_arr->value_offset(i + 1);

          const auto& sv_map =
              sim.state_variables(first_scene_index(), phase_index);

          struct ResolvedCoeff
          {
            ColIndex col;
            double coeff;
          };
          std::vector<ResolvedCoeff> resolved_coeffs;
          resolved_coeffs.reserve(static_cast<size_t>(list_end - list_start));

          for (int64_t k = list_start; k < list_end; ++k) {
            const auto key_view = coeff_key_arr->GetView(k);
            const auto coeff = coeff_val_arr->Value(k);

            // Parse structured key: "class:var:uid"
            const auto c1 = key_view.find(':');
            if (c1 == std::string_view::npos) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: malformed coeff key '{}' at iter "
                  "{}; skipping coefficient",
                  key_view,
                  cut_iter_idx);
              continue;
            }
            const auto c2 = key_view.find(':', c1 + 1);
            if (c2 == std::string_view::npos) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: malformed coeff key '{}' at iter "
                  "{}; skipping coefficient",
                  key_view,
                  cut_iter_idx);
              continue;
            }
            const auto cls = key_view.substr(0, c1);
            const auto var = key_view.substr(c1 + 1, c2 - c1 - 1);
            const auto uid_str = key_view.substr(c2 + 1);
            int uid_val = 0;
            const auto* const end_ptr = uid_str.data() + uid_str.size();
            const auto [ptr, ec] = std::from_chars(
                // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
                uid_str.data(),
                end_ptr,
                uid_val);
            if (ec != std::errc {}) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: invalid uid '{}' in key '{}' "
                  "at iter {}; skipping coefficient",
                  uid_str,
                  key_view,
                  cut_iter_idx);
              continue;
            }

            bool found = false;
            for (const auto& [skey, svar] : sv_map) {
              if (skey.class_name == cls && skey.col_name == var
                  && skey.uid == Uid {uid_val})
              {
                resolved_coeffs.push_back({.col = svar.col(), .coeff = coeff});
                found = true;
                break;
              }
            }
            if (!found) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: structured key '{}' not found in "
                  "state variables at iter {}; ignoring coefficient",
                  key_view,
                  cut_iter_idx);
            }
          }

          // Apply the loader-side noise filter (same logic as CSV path).
          if (!resolved_coeffs.empty()) {
            const auto cut_coeff_eps =
                planning_lp.options().sddp_cut_coeff_eps();
            double row_max = 0.0;
            for (const auto& rc : resolved_coeffs) {
              row_max = std::max(row_max, std::abs(rc.coeff));
            }
            const double drop_threshold = cut_coeff_eps * row_max;
            if (drop_threshold > 0.0) {
              std::erase_if(resolved_coeffs,
                            [drop_threshold](const ResolvedCoeff& rc)
                            { return std::abs(rc.coeff) < drop_threshold; });
            }
          }

          // Build the SparseRow template.
          auto row = SparseRow {
              .lowb = rhs,
              .uppb = LinearProblem::DblMax,
              .variable_uid = phase_uid,
          };
          sddp_loaded_cut_tag.apply_to(row);

          // Preserve the original ``extra`` discriminator (from the
          // parquet column).  Falls back to ``result.count`` for
          // legacy files that lack the ``extra`` column — preserves
          // per-load uniqueness even when on-disk metadata is
          // incomplete.
          const auto cut_offset = extra_arr ? cut_extra : result.count;
          const auto phase_uid_ctx = sim.uid_of(phase_index);

          // Route the cut to the specific scene it was generated for
          // (read from the ``scene`` column).  Falls back to
          // broadcasting across all scenes when the column is absent
          // (legacy parquet schema).  Without this routing each cut
          // would be replicated to all 16 scenes, multiplying both
          // the LP row count and the cut-store size by N_scenes at
          // every level transition — observed as 6 400 → 104 800 →
          // 1 679 560 cuts across juan/IPLP's 3 cascade transitions.
          auto build_scene_row = [&](SceneIndex scene_idx)
          {
            auto scene_row = row;
            scene_row.context = make_iteration_context(sim.uid_of(scene_idx),
                                                       phase_uid_ctx,
                                                       uid_of(cut_iter_idx),
                                                       cut_offset);
            for (const auto& [col, coeff] : resolved_coeffs) {
              scene_row[col] = coeff;
            }
            return scene_row;
          };

          auto push_to_cell = [&](SceneIndex scene_idx, SparseRow&& scene_row)
          {
            auto& cell =
                accum[CellKey {.scene = scene_idx, .phase = phase_index}];
            if (cut_type == CutType::Optimality) {
              cell.opt.push_back(std::move(scene_row));
            } else {
              cell.feas.push_back(std::move(scene_row));
            }
          };

          if (scene_arr) {
            // Per-scene routing (the file has a ``scene`` column).
            const auto scene_uid_val = make_uid<Scene>(scene_arr->Value(i));
            // Resolve scene_uid → scene_index.  Skip the cut when the
            // uid doesn't match any scene in the current simulation
            // (e.g. saved from a run with a different scene_array).
            SceneIndex target_scene {};
            bool scene_found = false;
            for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes))
            {
              if (sim.uid_of(scene_index) == scene_uid_val) {
                target_scene = scene_index;
                scene_found = true;
                break;
              }
            }
            if (!scene_found) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: unknown scene UID {} in {}, "
                  "skipping cut at iter {}",
                  scene_uid_val,
                  fname,
                  cut_iter_idx);
              continue;
            }
            push_to_cell(target_scene, build_scene_row(target_scene));
          } else {
            // Legacy fallback: broadcast to every scene.
            for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes))
            {
              push_to_cell(scene_index, build_scene_row(scene_index));
            }
          }
          ++result.count;
        }
      }
    }

    // Bulk install per cell (mirrors the CSV loader's pass 2).
    //
    // When ``cut_store`` is non-null we also push each loaded cut into
    // the manager via ``store_cut`` so its per-scene vectors include
    // these inherited cuts.  Without this, downstream operations like
    // ``SDDPCutManager::forget_first_cuts(N)`` walk a store that
    // doesn't know about the loaded rows and delete the wrong LP rows
    // — root cause of the cascade ``forget`` + compress-mode crash.
    for (auto&& [cell_key, cell_cuts] : accum) {
      const auto [scene_index, phase_index] =
          std::pair {cell_key.scene, cell_key.phase};
      const auto scene_uid_val = sim.uid_of(scene_index);
      const auto phase_uid_val = sim.uid_of(phase_index);

      if (!cell_cuts.opt.empty()) {
        for (const auto& cut : cell_cuts.opt) {
          bound_alpha_for_cut(planning_lp, scene_index, phase_index, cut);
        }
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        // Capture base row index BEFORE add_rows so each cut's
        // assigned row index can be reconstructed as ``base + i``.
        const auto base_row =
            static_cast<std::size_t>(static_cast<int>(li.get_numrows()));
        li.add_rows(cell_cuts.opt);
        for (const auto& cut : cell_cuts.opt) {
          li.record_cut_row(cut);
        }
        if (cut_store != nullptr) {
          for (std::size_t i = 0; i < cell_cuts.opt.size(); ++i) {
            cut_store->store_cut(scene_index,
                                 phase_index,
                                 cell_cuts.opt[i],
                                 CutType::Optimality,
                                 RowIndex {static_cast<int>(base_row + i)},
                                 scene_uid_val,
                                 phase_uid_val);
          }
        }
      }

      if (!cell_cuts.feas.empty()) {
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        const auto base_row =
            static_cast<std::size_t>(static_cast<int>(li.get_numrows()));
        li.add_rows(cell_cuts.feas);
        for (const auto& cut : cell_cuts.feas) {
          li.record_cut_row(cut);
        }
        if (cut_store != nullptr) {
          for (std::size_t i = 0; i < cell_cuts.feas.size(); ++i) {
            cut_store->store_cut(scene_index,
                                 phase_index,
                                 cell_cuts.feas[i],
                                 CutType::Feasibility,
                                 RowIndex {static_cast<int>(base_row + i)},
                                 scene_uid_val,
                                 phase_uid_val);
          }
        }
      }
    }

    SPDLOG_TRACE("SDDP: loaded {} cuts from {} parquet file(s)",
                 result.count,
                 files.size());
    return result;
  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message =
            std::format("Error loading cuts from {}: {}", filepath, e.what()),
    });
  }
}

[[nodiscard]] auto load_scene_cuts_from_directory_parquet(
    PlanningLP& planning_lp,
    const std::string& directory,
    double scale_alpha,
    const LabelMaker& label_maker,
    const StrongIndexVector<SceneIndex,
                            StrongIndexVector<PhaseIndex, PhaseStateInfo>>*
        scene_phase_states) -> std::expected<CutLoadResult, Error>
{
  CutLoadResult total {};

  if (!std::filesystem::exists(directory)) {
    return total;  // No directory = no cuts to load (not an error)
  }

  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto filename = entry.path().filename().string();

    // Skip error files from infeasible scenes (previous runs).
    if (filename.starts_with("error_")) {
      SPDLOG_INFO("SDDP hot-start: skipping error file {}", filename);
      continue;
    }

    // Only consider `scene_*.parquet` plus the combined `sddp_cuts.parquet`.
    // `*.append-*.parquet` siblings of the combined file are handled
    // internally by `load_cuts_parquet`, so we only invoke the loader on
    // the primary stem here.
    if (!filename.ends_with(".parquet")) {
      continue;
    }
    if (filename.contains(".append-")) {
      continue;  // picked up automatically by the primary loader
    }
    if (!filename.starts_with("scene_") && filename != sddp_file::combined_cuts)
    {
      continue;
    }

    auto result = load_cuts_parquet(planning_lp,
                                    entry.path().string(),
                                    scale_alpha,
                                    label_maker,
                                    scene_phase_states);
    if (result.has_value()) {
      total.count += result->count;
      total.max_iteration =
          std::max(total.max_iteration, result->max_iteration);
      SPDLOG_TRACE(
          "SDDP hot-start: loaded {} cuts from {}", result->count, filename);
    } else {
      SPDLOG_WARN("SDDP hot-start: could not load {}: {}",
                  filename,
                  result.error().message);
    }
  }

  return total;
}

}  // namespace gtopt
