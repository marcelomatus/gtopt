/**
 * @file      sddp_cut_parquet.cpp
 * @brief     Apache Parquet save / load for SDDP cuts.
 * @date      2026-05-11
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Sibling of `sddp_cut_csv.cpp` and `sddp_cut_json.cpp`.  Uses a typed
 * Arrow schema with a `list<struct<key,val>>` column for cut
 * coefficients — eliminating the variable-schema CSV tail and the
 * `{:.17g}` text-formatting dance.  Bit-exactness comes free from
 * float64 storage in Parquet.
 *
 * On-disk schema:
 *     type:      utf8        ("o" or "f")
 *     phase:     int32
 *     scene:     int32
 *     name:      utf8
 *     iteration: int32       (extracted from cut name; explicit for fast scan)
 *     rhs:       float64
 *     dual:      float64?    (nullable)
 *     coeffs:    list<struct<key: utf8, val: float64>>
 *
 * File metadata: KeyValueMetadata{version = "2", scale_objective = "<.17g>"}.
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

// NOLINTBEGIN(readability-trailing-comma, performance-unnecessary-value-param,
// performance-unnecessary-copy-initialization, misc-const-correctness,
// modernize-use-designated-initializers)

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
  return arrow::schema(
      {
          arrow::field("type", arrow::utf8(), /*nullable=*/false),
          arrow::field("phase", arrow::int32(), /*nullable=*/false),
          arrow::field("scene", arrow::int32(), /*nullable=*/false),
          arrow::field("name", arrow::utf8(), /*nullable=*/false),
          arrow::field("iteration", arrow::int32(), /*nullable=*/false),
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

  arrow::StringBuilder type_b {pool};
  arrow::Int32Builder phase_b {pool};
  arrow::Int32Builder scene_b {pool};
  arrow::StringBuilder name_b {pool};
  arrow::Int32Builder iter_b {pool};
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
      std::vector<std::shared_ptr<arrow::ArrayBuilder>> {coeff_key_b,
                                                         coeff_val_b});
  arrow::ListBuilder coeff_list_b {pool, coeff_struct_b};

  const auto reserve_status = [&](arrow::Status s) -> std::expected<void, Error>
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
  if (auto e = reserve_status(name_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(iter_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(rhs_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }
  if (auto e = reserve_status(dual_b.Reserve(std::ssize(cuts))); !e) {
    return std::unexpected(e.error());
  }

  for (const auto& cut : cuts) {
    const std::string_view type_str =
        (cut.type == CutType::Feasibility) ? "f" : "o";
    if (auto s = type_b.Append(type_str); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = phase_b.Append(static_cast<int32_t>(cut.phase_uid)); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = scene_b.Append(static_cast<int32_t>(cut.scene_uid)); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = name_b.Append(cut.name); !s.ok()) {
      return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
    }
    if (auto s = iter_b.Append(static_cast<int32_t>(
            extract_iteration_from_name(std::string_view {cut.name})));
        !s.ok())
    {
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
            std::format("cut '{}' references col {} that is not a registered "
                        "state variable",
                        cut.name,
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
  std::shared_ptr<arrow::Array> name_a;
  std::shared_ptr<arrow::Array> iter_a;
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
  if (auto s = name_b.Finish(&name_a); !s.ok()) {
    return std::unexpected(make_io_error(filepath_for_error, s.ToString()));
  }
  if (auto s = iter_b.Finish(&iter_a); !s.ok()) {
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
      {type_a, phase_a, scene_a, name_a, iter_a, rhs_a, dual_a, coeffs_a});
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
    auto out = *open_result;

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
    auto out = *open_result;

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
  std::shared_ptr<arrow::io::RandomAccessFile> input = *open_result;

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
        StrongIndexVector<PhaseIndex, PhaseStateInfo>>* scene_phase_states)
    -> std::expected<CutLoadResult, Error>
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

    // De-dup across {phase_uid, name} — the same cut may appear in
    // multiple files (e.g. combined + append-deltas) and in multiple
    // scene rows of one file.  We install each cut once and broadcast
    // it to every scene.
    std::set<std::pair<PhaseUid, std::string>> loaded_keys;

    for (const auto& fname : files) {
      auto table_result = read_parquet_table(fname);
      if (!table_result.has_value()) {
        return std::unexpected(table_result.error());
      }
      auto& table = *table_result;

      auto type_col = table->GetColumnByName("type");
      auto phase_col = table->GetColumnByName("phase");
      auto name_col = table->GetColumnByName("name");
      auto rhs_col = table->GetColumnByName("rhs");
      auto coeffs_col = table->GetColumnByName("coeffs");
      if (!type_col || !phase_col || !name_col || !rhs_col || !coeffs_col) {
        return std::unexpected(make_io_error(
            fname,
            "missing one of required columns (type/phase/name/rhs/coeffs)"));
      }

      for (int chunk_i = 0; chunk_i < type_col->num_chunks(); ++chunk_i) {
        auto type_arr = std::dynamic_pointer_cast<arrow::StringArray>(
            type_col->chunk(chunk_i));
        auto phase_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
            phase_col->chunk(chunk_i));
        auto name_arr = std::dynamic_pointer_cast<arrow::StringArray>(
            name_col->chunk(chunk_i));
        auto rhs_arr = std::dynamic_pointer_cast<arrow::DoubleArray>(
            rhs_col->chunk(chunk_i));
        auto coeffs_arr = std::dynamic_pointer_cast<arrow::ListArray>(
            coeffs_col->chunk(chunk_i));
        if (!type_arr || !phase_arr || !name_arr || !rhs_arr || !coeffs_arr) {
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

        const auto nrows = type_arr->length();
        for (int64_t i = 0; i < nrows; ++i) {
          const CutType cut_type = (type_arr->GetView(i) == "f")
              ? CutType::Feasibility
              : CutType::Optimality;
          const auto phase_uid = make_uid<Phase>(phase_arr->Value(i));
          const auto cut_name = std::string {name_arr->GetView(i)};
          const auto rhs = rhs_arr->Value(i);

          auto pit = phase_uid_to_index.find(phase_uid);
          if (pit == phase_uid_to_index.end()) {
            SPDLOG_WARN(
                "SDDP load_cuts_parquet: unknown phase UID {} in {}, "
                "skipping cut '{}'",
                phase_uid,
                fname,
                cut_name);
            continue;
          }
          const auto phase_index = pit->second;

          if (!loaded_keys.emplace(phase_uid, cut_name).second) {
            continue;  // already seen (combined + append, or per-scene rows)
          }

          result.max_iteration = std::max(
              result.max_iteration,
              extract_iteration_from_name(std::string_view {cut_name}));

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
                  "SDDP load_cuts_parquet: malformed coeff key '{}' in cut "
                  "'{}'; skipping coefficient",
                  key_view,
                  cut_name);
              continue;
            }
            const auto c2 = key_view.find(':', c1 + 1);
            if (c2 == std::string_view::npos) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: malformed coeff key '{}' in cut "
                  "'{}'; skipping coefficient",
                  key_view,
                  cut_name);
              continue;
            }
            const auto cls = key_view.substr(0, c1);
            const auto var = key_view.substr(c1 + 1, c2 - c1 - 1);
            const auto uid_str = key_view.substr(c2 + 1);
            int uid_val = 0;
            const auto [ptr, ec] =
                std::from_chars(uid_str.data(),
                                uid_str.data() + uid_str.size(),  // NOLINT
                                uid_val);
            if (ec != std::errc {}) {
              SPDLOG_WARN(
                  "SDDP load_cuts_parquet: invalid uid '{}' in key '{}' "
                  "for cut '{}'; skipping coefficient",
                  uid_str,
                  key_view,
                  cut_name);
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
                  "state variables for cut '{}'; ignoring coefficient",
                  key_view,
                  cut_name);
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

          // Build the SparseRow template and broadcast to all scenes.
          auto row = SparseRow {
              .lowb = rhs,
              .uppb = LinearProblem::DblMax,
              .variable_uid = phase_uid,
          };
          sddp_loaded_cut_tag.apply_to(row);

          const auto cut_iter =
              extract_iteration_from_name(std::string_view {cut_name});
          const auto cut_offset = result.count;
          const auto phase_uid_ctx = sim.uid_of(phase_index);
          for (const auto scene_index : iota_range<SceneIndex>(0, num_scenes)) {
            auto scene_row = row;
            scene_row.context = make_iteration_context(sim.uid_of(scene_index),
                                                       phase_uid_ctx,
                                                       uid_of(cut_iter),
                                                       cut_offset);
            for (const auto& [col, coeff] : resolved_coeffs) {
              scene_row[col] = coeff;
            }
            auto& cell = accum[CellKey {scene_index, phase_index}];
            if (cut_type == CutType::Optimality) {
              cell.opt.push_back(std::move(scene_row));
            } else {
              cell.feas.push_back(std::move(scene_row));
            }
          }
          ++result.count;
        }
      }
    }

    // Bulk install per cell (mirrors the CSV loader's pass 2).
    for (auto&& [cell_key, cell_cuts] : accum) {
      const auto [scene_index, phase_index] =
          std::pair {cell_key.scene, cell_key.phase};

      if (!cell_cuts.opt.empty()) {
        for (const auto& cut : cell_cuts.opt) {
          bound_alpha_for_cut(planning_lp, scene_index, phase_index, cut);
        }
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        li.add_rows(cell_cuts.opt);
        for (const auto& cut : cell_cuts.opt) {
          li.record_cut_row(cut);
        }
      }

      if (!cell_cuts.feas.empty()) {
        auto& li =
            planning_lp.system(scene_index, phase_index).linear_interface();
        li.add_rows(cell_cuts.feas);
        for (const auto& cut : cell_cuts.feas) {
          li.record_cut_row(cut);
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

// NOLINTEND(readability-trailing-comma, performance-unnecessary-value-param,
// performance-unnecessary-copy-initialization, misc-const-correctness,
// modernize-use-designated-initializers)
