/**
 * @file      output_context.cpp
 * @brief     Implementation of output context for Parquet/CSV writing
 * @date      Wed Mar 26 01:05:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the OutputContext class methods for writing LP
 * solution results to Parquet and CSV files using Arrow builders.
 */

#include <algorithm>  // For std::find
#include <array>
#include <concepts>
#include <filesystem>
#include <format>
#include <fstream>
#include <semaphore>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <arrow/io/compressed.h>
#include <arrow/util/compression.h>
#include <gtopt/map_reserve.hpp>
#include <gtopt/output_context.hpp>
#include <parquet/arrow/writer.h>
#include <parquet/types.h>
#include <spdlog/spdlog.h>

template<typename T>
concept ArrowBuildable = requires {
  {
    arrow::CTypeTraits<T>::type_singleton()
  } -> std::same_as<std::shared_ptr<arrow::DataType>>;
};

namespace
{

using namespace gtopt;

template<ArrowBuildable Type = arrow::DoubleType,
         typename Values,
         typename Valids = std::vector<bool>>
auto make_array(Values&& values, Valids&& valids = {})
{
  typename arrow::CTypeTraits<Type>::BuilderType builder;

  const auto st = valids.empty()
      ? builder.AppendValues(std::forward<Values>(values))
      : builder.AppendValues(std::forward<Values>(values),
                             std::forward<Valids>(valids));
  if (!st.ok()) {
    SPDLOG_CRITICAL("Cannot append values: {}", st.ToString());
  }

  ArrowArray array;
  const auto fs = builder.Finish(&array);
  if (!fs.ok()) {
    SPDLOG_CRITICAL("Cannot build values: {}", fs.ToString());
  }

  return array;
}

using str = std::string;

template<typename Type = Uid, typename STBUids>
  requires std::same_as<std::remove_cvref_t<STBUids>, gtopt::STBUids>
[[nodiscard]] auto make_stb_prelude(STBUids&& stb_uids)
{
  std::vector<ArrowField> fields = {
      arrow::field(str {Scenario::class_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Stage::class_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Block::class_name}, ArrowTraits<Type>::type()),
  };

  // Capture fields before moving stb_uids
  auto&& f_uids = std::forward<STBUids>(stb_uids);
  std::vector<ArrowArray> arrays = {
      make_array<Type>(f_uids.scenario_uids),
      make_array<Type>(f_uids.stage_uids),
      make_array<Type>(f_uids.block_uids),
  };

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = Uid, typename STUids>
  requires std::same_as<std::remove_cvref_t<STUids>, gtopt::STUids>
[[nodiscard]] auto make_st_prelude(STUids&& st_uids)
{
  std::vector<ArrowField> fields = {
      arrow::field(str {Scenario::class_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Stage::class_name}, ArrowTraits<Type>::type()),
  };

  auto&& f_uids = std::forward<STUids>(st_uids);
  std::vector<ArrowArray> arrays = {
      make_array<Type>(f_uids.scenario_uids),
      make_array<Type>(f_uids.stage_uids),
  };

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = Uid, typename TUids>
  requires std::same_as<std::remove_cvref_t<TUids>, gtopt::TUids>
[[nodiscard]] auto make_t_prelude(TUids&& t_uids)
{
  std::vector<ArrowField> fields = {
      arrow::field(str {Stage::class_name}, ArrowTraits<Type>::type()),
  };

  std::vector<ArrowArray> arrays = {
      make_array<Type>(std::forward<TUids>(t_uids).stage_uids),
  };

  return std::pair {std::move(fields), std::move(arrays)};
}

// Round `v` to `digits` decimal places.  Inline / branch-free hot path
// — called per cell value when `output_round_decimals > 0`.  The bound
// `digits <= 15` covers every reasonable case (double has ~15-17
// significant decimal digits) and lets us precompute `10^digits` from
// a small lookup table to avoid a per-call `std::pow`.
[[nodiscard]] inline double round_to_digits(double v, int digits) noexcept
{
  if (digits <= 0) [[unlikely]] {
    return v;
  }
  static constexpr std::array<double, 16> kScales = {
      1.0,
      1e1,
      1e2,
      1e3,
      1e4,
      1e5,
      1e6,
      1e7,
      1e8,
      1e9,
      1e10,
      1e11,
      1e12,
      1e13,
      1e14,
      1e15,
  };
  const auto idx = static_cast<std::size_t>(digits < 15 ? digits : 15);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  const double scale = kScales[idx];
  return std::round(v * scale) / scale;
}

// Parse the trailing integer in a field name like `uid:42` → 42.  Returns
// `-1` when the prefix is missing — those fields are written as a synthetic
// `uid=-1` row in the long output so they survive without breaking the
// schema.
[[nodiscard]] inline auto parse_uid_suffix(std::string_view fname) noexcept
    -> int64_t
{
  constexpr std::string_view kPrefix {"uid:"};
  if (!fname.starts_with(kPrefix) || fname.size() <= kPrefix.size()) {
    return -1;
  }
  int64_t out = 0;
  for (auto c : fname.substr(kPrefix.size())) {
    if (c < '0' || c > '9') {
      return -1;
    }
    out = (out * 10) + (c - '0');
  }
  return out;
}

// The (only) solution-table writer.  Produces a 6-column long table:
//   `(scenario, stage, block, uid, value, valid)`
// with one row per *non-zero* cell across every uid column in the field
// vector.  POC measurements on `support/plp/2_years` showed 5-7× smaller
// per-partition files on the heavy Generator streams (0.1 % cell density)
// and 2-3× on the LMP / line-flow streams.
//
// Sign convention: an exact zero is dropped.  Absence of a row means "no
// contribution from this uid in this cell", which is the natural reader
// semantic (sums and min/max aggregations are invariant under dropped
// zeros).  Wide output was removed — convert long files externally if a real
// zero must be materialised per cell.
template<typename Type = double, typename FieldVector>
auto make_field_arrays_long(FieldVector&& field_vector, int round_digits = 0)
{
  // Pre-pass: extract the prelude's (scenario, stage, block) arrays once;
  // we'll index them per-row to build the long output.
  //
  // Identifier columns use `uint16_t` (0-65535).  gtopt's per-class uid
  // namespace caps far below 64K — typical 2-year case has ~2000
  // generators, ~330 lines, ~10 reservoirs — and the per-cell axes
  // are even smaller (≤16 scenarios, ≤51 stages, ≤510 blocks).  Two
  // bytes per row instead of four (int32) or eight (int64) cuts the
  // identifier byte budget by 2-4× on the typical long-form table
  // before any encoding/compression runs.  See feedback message
  // 2026-05-19 from MM (`gtopt_2_years` ~1 GB output target).
  // Value-column type specialization (2026-05-19):
  //
  //   * `round_digits ∈ [1, 7]` → store as `float` (4 bytes/value).
  //     Float32's ~7 significant decimal digits cover any rounding
  //     target in this range *exactly*; an explicit `round_to_digits`
  //     is redundant because the cast to float already trims the
  //     bottom mantissa bits to ~7-digit precision.  Halves the raw
  //     value-column footprint before `BYTE_STREAM_SPLIT + zstd`
  //     does its thing.
  //
  //   * `round_digits ≥ 8` → keep `double` (8 bytes/value) and
  //     `round_to_digits` explicitly.  Float32 cannot represent 8+
  //     decimals losslessly, so the user is asking for double-grade
  //     precision and we honour it.
  //
  //   * `round_digits ≤ 0` → keep `double` with no rounding.  The
  //     "no rounding, verify all digits" opt-out.
  //
  // Boolean below threads `use_float32` into the per-row loop and
  // the column-type selector.  Implementation note: we keep the
  // template `Type` parameter wired through for callers that want
  // double explicitly (today every caller passes `double`), and
  // overlay a runtime flag for the float32 specialization — adding
  // a second template instantiation would double compile time on
  // every TU that calls `make_field_arrays_long`.
  const bool use_float32 = (round_digits >= 1 && round_digits <= 7);
  std::vector<uint16_t> long_scenario;
  std::vector<uint16_t> long_stage;
  std::vector<uint16_t> long_block;
  std::vector<uint16_t> long_uid;
  std::vector<Type> long_value;
  std::vector<float> long_value_f32;
  // Optional null mask — only populated if any field carried valids.
  const std::vector<bool> long_valid;
  const bool any_invalid = false;

  // Cache the prelude row vectors as plain ints so the per-row indexing
  // below avoids re-walking the Arrow arrays.  Falls back to empty
  // vectors when no field provides a prelude (degenerate case).
  std::vector<int32_t> prelude_scenario;
  std::vector<int32_t> prelude_stage;
  std::vector<int32_t> prelude_block;
  bool prelude_loaded = false;

  const auto load_prelude = [&](const auto& parrays)
  {
    // parrays is std::vector<ArrowArray>, ordering matches make_*_prelude.
    if (parrays.empty()) {
      return;
    }
    auto extract_int = [](const ArrowArray& arr) -> std::vector<int32_t>
    {
      std::vector<int32_t> out;
      const auto len = arr->length();
      out.reserve(static_cast<std::size_t>(len));
      // Dispatch on the actual array type — most preludes use int32
      // (matching `Uid` size on this build), but `make_stb_prelude` may
      // emit int64; cover both.
      if (auto* a32 = dynamic_cast<arrow::Int32Array*>(arr.get())) {
        for (int64_t i = 0; i < len; ++i) {
          out.push_back(a32->IsNull(i) ? 0 : a32->Value(i));
        }
      } else if (auto* a64 = dynamic_cast<arrow::Int64Array*>(arr.get())) {
        for (int64_t i = 0; i < len; ++i) {
          out.push_back(a64->IsNull(i) ? 0
                                       : static_cast<int32_t>(a64->Value(i)));
        }
      } else {
        // Unknown integer type — leave a zero-filled vector so the long
        // output still has the right row count.
        out.assign(static_cast<std::size_t>(len), 0);
      }
      return out;
    };

    // make_stb_prelude order: scenario, stage, block.
    // make_st_prelude order:  scenario, stage.
    // make_t_prelude order:   stage.
    if (parrays.size() >= 3) {
      prelude_scenario = extract_int(parrays[0]);
      prelude_stage = extract_int(parrays[1]);
      prelude_block = extract_int(parrays[2]);
    } else if (parrays.size() == 2) {
      prelude_scenario = extract_int(parrays[0]);
      prelude_stage = extract_int(parrays[1]);
      prelude_block.assign(prelude_stage.size(), 0);
    } else if (parrays.size() == 1) {
      prelude_stage = extract_int(parrays[0]);
      prelude_scenario.assign(prelude_stage.size(), 0);
      prelude_block.assign(prelude_stage.size(), 0);
    }
    prelude_loaded = true;
  };

  for (auto&& field_data : std::forward<FieldVector>(field_vector)) {
    auto&& [fname, fvalues, fvalids, prelude] =
        std::forward<decltype(field_data)>(field_data);

    if (fvalues.empty()) [[unlikely]] {
      continue;
    }

    if (!prelude_loaded && prelude) [[likely]] {
      const auto& [pfields, parrays] = *prelude;
      load_prelude(parrays);
    }

    const int64_t uid_val = parse_uid_suffix(fname);

    const auto n_rows = std::min(fvalues.size(), prelude_stage.size());
    const bool has_valids = !fvalids.empty();

    for (std::size_t i = 0; i < n_rows; ++i) {
      const bool is_valid = !has_valids || fvalids[i];
      if (!is_valid) {
        // Drop invalid cells entirely — the long form omits the row and
        // downstream sums treat absence as zero.
        continue;
      }
      const Type v = static_cast<Type>(fvalues[i]);
      // Drop exact zeros to keep the file small on the typical 0.1 %
      // dense gtopt output.  See function docstring above for the
      // semantic implications.
      if (v == Type {0}) {
        continue;
      }
      // Narrowing to uint16 is safe: see the `long_scenario` declaration
      // comment above for the per-axis bound rationale.  Values
      // outside 0-65535 are wrapped silently — relying on the bound
      // check above the loop (and on `parse_uid_suffix` returning
      // -1 → 65535 sentinel) for correctness.
      long_scenario.push_back(static_cast<uint16_t>(
          prelude_scenario.empty() ? 0 : prelude_scenario[i]));
      long_stage.push_back(
          static_cast<uint16_t>(prelude_stage.empty() ? 0 : prelude_stage[i]));
      long_block.push_back(
          static_cast<uint16_t>(prelude_block.empty() ? 0 : prelude_block[i]));
      long_uid.push_back(static_cast<uint16_t>(uid_val));
      // Round in double-precision first, *then* narrow.  Float32 has
      // ~7 *binary*-significant digits; that is not the same as a
      // decimal-grid round.  Without the explicit round_to_digits
      // step the float32 holds whatever 24-bit mantissa is closest
      // to the full-precision double — the low mantissa bytes vary
      // chaotically across consecutive samples and BYTE_STREAM_SPLIT
      // cannot exploit a shared decimal alignment.  Rounding first
      // lands every value on the requested decimal grid, then the
      // cast picks the nearest float32 to that grid point.  Result:
      // consecutive samples share many more high bits → better BSS
      // compression.
      const double rounded =
          round_to_digits(static_cast<double>(v), round_digits);
      if (use_float32) {
        long_value_f32.push_back(static_cast<float>(rounded));
      } else {
        long_value.push_back(static_cast<Type>(rounded));
      }
    }
  }

  (void)any_invalid;
  (void)long_valid;

  // Note for future readers: we deliberately do NOT sort the
  // accumulated rows.  The natural insertion order is already
  // (uid, scenario, stage, block) — each outer field iteration
  // emits one uid's worth of rows in the prelude's order, which is
  // (scenario, stage, block).  That order is compression-friendly:
  // long monotonic `uid` runs (3 600 rows per uid on the 2-year
  // case), and within each uid block the `value` column is a
  // single-scenario timeline (consecutive blocks have correlated
  // dispatch → BSS finds shared high bytes).
  //
  // A re-sort by (uid, stage, block) was tried on 2026-05-19 and
  // INCREASED the disk size from 265 MB to 274 MB on the 2-year
  // case — that order interleaves scenarios within each (stage,
  // block) slot, breaking the within-scenario value smoothness.
  // If you ever consider re-introducing a sort, measure on a real
  // case first; the natural order is the right one for this data.

  std::vector<ArrowField> fields {
      arrow::field("scenario", arrow::uint16()),
      arrow::field("stage", arrow::uint16()),
      arrow::field("block", arrow::uint16()),
      arrow::field("uid", arrow::uint16()),
      arrow::field("value",
                   use_float32 ? arrow::float32() : ArrowTraits<Type>::type()),
  };
  std::vector<ArrowArray> arrays {
      make_array<uint16_t>(long_scenario),
      make_array<uint16_t>(long_stage),
      make_array<uint16_t>(long_block),
      make_array<uint16_t>(long_uid),
      use_float32 ? make_array<float>(long_value_f32)
                  : make_array<Type>(long_value),
  };

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = double, typename FieldVector>
auto make_table(FieldVector&& field_vector, int round_digits = 0)
    -> arrow::Result<std::shared_ptr<arrow::Table>>
{
  // Output is long-only: one row per non-zero (scenario, stage, block, uid)
  // cell.  Wide output is no longer supported — convert long files externally
  // if a wide shape is needed.
  auto [fields, arrays] = make_field_arrays_long<Type>(
      std::forward<FieldVector>(field_vector), round_digits);

  return arrow::Table::Make(arrow::schema(std::move(fields)),
                            std::move(arrays));
}

// Translate a pre-validated codec name to a parquet::Compression enum value.
//
// The codec has already been probed for availability by probe_parquet_codec()
// at program startup and stored in Options::output_compression.  This
// function only does the name→enum translation; no runtime availability
// check is needed here.
[[nodiscard]] auto resolve_parquet_codec(std::string_view zfmt)
{
  using codec_t = decltype(parquet::Compression::UNCOMPRESSED);
  // Small fixed table — linear scan is cheaper than a hash lookup and
  // avoids constructing a std::string from the string_view key.
  //
  // Keep in lock-step with `probe_parquet_codec` (see below) and with
  // the JSON-side `CompressionCodec` enum in `planning_enums.hpp`.
  // Pre-2026-05-19 this table was missing entries for `snappy`, `lz4`,
  // `lz4_raw`, and `brotli`: the codecs all parsed clean in
  // `probe_parquet_codec` (so the log printed `Output compression
  // codec: lz4`) but `resolve_parquet_codec` then fell off the loop
  // and returned UNCOMPRESSED.  Result: every parquet column was
  // written uncompressed even though the user had asked for snappy
  // or lz4.  See the support/plp/2_years bloat regression.
  static constexpr std::array<std::pair<std::string_view, codec_t>, 9>
      codec_map {
          {
              {"", parquet::Compression::UNCOMPRESSED},
              {"none", parquet::Compression::UNCOMPRESSED},
              {"uncompressed", parquet::Compression::UNCOMPRESSED},
              {"snappy", parquet::Compression::SNAPPY},
              {"gzip", parquet::Compression::GZIP},
              {"brotli", parquet::Compression::BROTLI},
              {"zstd", parquet::Compression::ZSTD},
              // Parquet rejects `LZ4_FRAME` (an Arrow-level frame
              // wrapper) — use `LZ4` (the legacy Hadoop variant) for
              // the JSON/CLI keyword `lz4`, which parquet writers
              // accept.  The "raw" parquet LZ4 codec is `LZ4_RAW` but
              // it is not in the Arrow enum at this version, and
              // `LZ4` is the codec downstream Spark / Arrow readers
              // expect for any "lz4"-tagged file.
              {"lz4", parquet::Compression::LZ4},
              {"lzo", parquet::Compression::LZO},
          },
      };
  for (const auto& [name, codec] : codec_map) {
    if (name == zfmt) {
      return codec;
    }
  }
  return parquet::Compression::UNCOMPRESSED;
}

auto parquet_write_table(const auto& fpath, const auto& table, const auto& zfmt)
    -> arrow::Status
{
  const auto filename = std::format("{}.parquet", fpath.string());
  auto maybe_output = arrow::io::FileOutputStream::Open(filename);
  if (!maybe_output.ok()) {
    SPDLOG_CRITICAL("Cannot open Parquet output file '{}': {}",
                    filename,
                    maybe_output.status().ToString());
    return maybe_output.status();
  }
  auto& output = maybe_output.ValueUnsafe();

  parquet::WriterProperties::Builder props_builder;
  props_builder.compression(resolve_parquet_codec(zfmt));
  // Trim per-column parquet footer metadata.  Two on-by-default features
  // add per-column footer bytes for no benefit here:
  //
  //   * `disable_statistics()` — drops per-column min/max +
  //     null-count stats (~100 bytes per column).
  //   * `set_page_index_enabled(false)` — drops the per-column
  //     `OffsetIndex` / `ColumnIndex` blobs (another ~100 bytes per
  //     column).
  //
  // No downstream consumer (gtopt's scripts, marginal_units,
  // check_output, results_summary, compare) uses parquet column stats or
  // page indexes for pruning, and pyarrow's reader transparently handles
  // their absence.
  props_builder.disable_statistics();
  props_builder.disable_write_page_index();

  // Long-form `value` column: switch to BYTE_STREAM_SPLIT encoding +
  // disable dictionary on this column specifically.  Rationale:
  //
  //   * BYTE_STREAM_SPLIT reorders the bytes of consecutive
  //     little-endian IEEE-754 doubles into 8 separate streams (all
  //     byte-0 first, all byte-1 second, …).  Consecutive doubles in
  //     gtopt output have very similar exponents and high mantissas,
  //     so the byte-0/byte-1 streams compress 3-5× better under zstd
  //     than the interleaved layout (which is what PLAIN encoding
  //     gives).  Dictionary encoding hurts the long-form `value`
  //     column because every row has a distinct float — every value
  //     produces its own dictionary entry, doubling the page size
  //     before the codec runs.
  //
  // The `encoding(path, ...)` call is a harmless no-op for any table that
  // happens to lack a `"value"` column (e.g. an index-only prelude), and
  // Arrow accepts the property silently.  See also `make_field_arrays_long`
  // for the matching round-to-1e-8 step that maximises the benefit (more
  // zero bytes in the mantissa tail → better zstd ratio).
  props_builder.encoding("value", parquet::Encoding::BYTE_STREAM_SPLIT);
  props_builder.disable_dictionary("value");

  const auto props = props_builder.build();

  auto status = parquet::arrow::WriteTable(*table.get(),
                                           arrow::default_memory_pool(),
                                           output,
                                           4 * 1024 * 1024,
                                           props);
  if (!status.ok()) {
    SPDLOG_CRITICAL(
        "Parquet file write failed for '{}': {}", filename, status.ToString());
  }
  return status;
}

auto csv_write_table_plain(const auto& fpath, const auto& table)
{
  const auto filename = std::format("{}.csv", fpath.string());
  ARROW_ASSIGN_OR_RAISE(auto output,
                        arrow::io::FileOutputStream::Open(filename));

  const auto write_options = arrow::csv::WriteOptions::Defaults();
  auto status = WriteCSV(*table.get(), write_options, output.get());
  if (!status.ok()) {
    SPDLOG_CRITICAL(
        "CSV file write failed for '{}': {}", filename, status.ToString());
  }
  return status;
}

auto csv_write_table_compressed(const auto& fpath,
                                const auto& table,
                                arrow::Compression::type compression,
                                const std::string& ext)
{
  const auto filename = std::format("{}.csv.{}", fpath.string(), ext);
  ARROW_ASSIGN_OR_RAISE(auto file_output,
                        arrow::io::FileOutputStream::Open(filename));
  ARROW_ASSIGN_OR_RAISE(auto codec, arrow::util::Codec::Create(compression));
  ARROW_ASSIGN_OR_RAISE(
      auto compressed_output,
      arrow::io::CompressedOutputStream::Make(codec.get(), file_output));

  const auto write_options = arrow::csv::WriteOptions::Defaults();
  ARROW_RETURN_NOT_OK(
      WriteCSV(*table.get(), write_options, compressed_output.get()));
  ARROW_RETURN_NOT_OK(compressed_output->Close());
  return file_output->Close();
}

auto csv_write_table(const auto& fpath, const auto& table, const auto& zfmt)
{
  // The codec has already been validated by probe_parquet_codec() at startup.
  if (zfmt.empty() || zfmt == "none" || zfmt == "uncompressed") {
    return csv_write_table_plain(fpath, table);
  }
  if (zfmt == "zstd") {
    return csv_write_table_compressed(
        fpath, table, arrow::Compression::ZSTD, "zst");
  }
  if (zfmt == "gzip") {
    return csv_write_table_compressed(
        fpath, table, arrow::Compression::GZIP, "gz");
  }
  // Unknown codec — should not reach here after probe_parquet_codec().
  SPDLOG_WARN("CSV compression '{}' is not recognised; writing uncompressed",
              zfmt);
  return csv_write_table_plain(fpath, table);
}

auto write_table(std::string_view fmt,
                 const auto& fpath,
                 const auto& table,
                 std::string_view zfmt)
{
  arrow::Status status;
  if (fmt == "parquet") {
    status = parquet_write_table(fpath, table, zfmt);
  } else {
    status = csv_write_table(fpath, table, zfmt);
  }

  return status;
}

template<typename Type = double>
auto create_tables(std::string_view fmt,
                   SceneUid scene_uid,
                   PhaseUid phase_uid,
                   auto&& output_directory,
                   auto&& field_vector_map,
                   int round_digits = 0)
{
  using PathTable =
      std::pair<std::filesystem::path, std::shared_ptr<arrow::Table>>;

  std::vector<PathTable> path_tables;

  const auto dirpath = std::filesystem::path(output_directory);
  const auto scene_part = std::format("scene={}", scene_uid);
  const auto phase_part = std::format("phase={}", phase_uid);
  const auto csv_shard_suffix = std::format("_s{}_p{}", scene_uid, phase_uid);

  for (auto&& [class_fname, vfields] : field_vector_map) {
    // Key is std::array<std::string_view, 3> = (cname, fname, sname).
    // The file-name stem is `fname_sname` — composed once here instead
    // of at every `add_field` call (previously `as_label(fname, sname)`
    // allocated per element).
    const auto& cname = class_fname[0];
    const auto fname_stem =
        std::format("{}_{}", class_fname[1], class_fname[2]);
    const auto mtable = make_table<Type>(vfields, round_digits);

    const auto cname_dir = dirpath / cname;

    // Parquet: hive-partitioned directory
    //   {cname}/{fname_stem}.parquet/scene=<N>/phase=<M>/part{.parquet}
    // CSV: per-(scene, phase) shard in the class directory
    //   {cname}/{fname_stem}_s<N>_p<M>{.csv,.csv.zst,.csv.gz}
    std::filesystem::path dir_to_create;
    std::filesystem::path fpath;
    if (fmt == "parquet") {
      dir_to_create =
          cname_dir / (fname_stem + ".parquet") / scene_part / phase_part;
      fpath = dir_to_create / "part";
    } else {
      fpath = cname_dir / (fname_stem + csv_shard_suffix);
      // A field-name stem may itself contain a path separator (e.g. a
      // `UserModel` capture filed under `<tag>/<name>`), so create the
      // shard's PARENT rather than just `cname_dir`.  With a plain stem
      // `parent_path()` IS `cname_dir`, so existing single-level outputs are
      // unaffected.
      dir_to_create = fpath.parent_path();
    }

    std::error_code ec;
    std::filesystem::create_directories(dir_to_create, ec);
    if (ec) {
      SPDLOG_CRITICAL("Cannot create output directory '{}': {}",
                      dir_to_create.string(),
                      ec.message());
      continue;
    }

    if (!mtable.ok()) {
      SPDLOG_CRITICAL("Cannot create table '{}/{}': {}",
                      cname,
                      fname_stem,
                      mtable.status().ToString());
      continue;
    }
    path_tables.emplace_back(std::move(fpath), *mtable);
  }

  return path_tables;
}

}  // namespace

namespace gtopt
{

std::string probe_parquet_codec(std::string_view requested)
{
  // Uncompressed / empty / "none": nothing to probe.
  if (requested.empty() || requested == "none" || requested == "uncompressed") {
    return std::string(requested);
  }

  // Map known codec names to Arrow compression types.
  using Compression = arrow::Compression;
  static const std::unordered_map<std::string_view, Compression::type>
      codec_map {
          {"gzip", Compression::GZIP},
          {"zstd", Compression::ZSTD},
          {"lzo", Compression::LZO},
          {"snappy", Compression::SNAPPY},
          {"brotli", Compression::BROTLI},
          {"lz4", Compression::LZ4_FRAME},
      };

  const auto it = codec_map.find(requested);
  if (it == codec_map.end()) {
    SPDLOG_WARN("Output compression '{}' is unknown; falling back to gzip",
                requested);
    if (arrow::util::Codec::IsAvailable(Compression::GZIP)) {
      return "gzip";
    }
    return "";
  }

  // arrow::util::Codec::IsAvailable() is the correct runtime check: it tests
  // whether the codec was actually compiled into the linked Arrow library,
  // unlike parquet::IsCodecSupported() which only validates the enum value.
  if (arrow::util::Codec::IsAvailable(it->second)) {
    spdlog::info("Output compression codec: {}", requested);
    return std::string(requested);
  }

  // Requested codec is absent from this Arrow build — fall back to gzip.
  SPDLOG_WARN(
      "Output compression '{}' is not supported by the linked Arrow library "
      "(codec not compiled in); falling back to gzip",
      requested);
  if (it->second != Compression::GZIP
      && arrow::util::Codec::IsAvailable(Compression::GZIP))
  {
    return "gzip";
  }

  SPDLOG_WARN(
      "Output compression '{}' and gzip are both unavailable; "
      "writing uncompressed output",
      requested);
  return "";
}

void OutputContext::write() const
{
  const auto fmt = options().output_format();
  const auto zfmt = options().output_compression();
  const auto round_digits = options().output_round_decimals();
  auto path_tables = create_tables(fmt,
                                   m_scene_uid_,
                                   m_phase_uid_,
                                   options().output_directory(),
                                   field_vector_map,
                                   round_digits);

  SPDLOG_DEBUG(
      "  Writing {} output tables to '{}' "
      "(scene={}, phase={}, format={}, compression={})",
      path_tables.size(),
      options().output_directory(),
      m_scene_uid_,
      m_phase_uid_,
      fmt,
      zfmt);

  // Per-cell shard-write throttle.  Earlier the per-(scene, phase)
  // ``OutputContext::write`` spawned one ``std::jthread`` per shard
  // (one per element class' parquet file).  Combined with the
  // parent ``PlanningLP::write_out`` cell pool (cpu_factor=1.0 →
  // ~20 cells in flight on a 20-core host) and ~55 shards per cell,
  // ~1100 jthreads piled into Arrow's default memory pool and
  // parquet encoders simultaneously, producing pathological lock
  // contention (observed on juan/IPLP: 60s+ per-cell ``oc.write``
  // wall time when the inner storm peaked).  A static
  // ``std::counting_semaphore`` bounds total in-flight shard writes
  // ACROSS every cell currently running ``write()`` — independent
  // of how many cells the parent pool dispatched in parallel.
  //
  // Slot count: ``hardware_concurrency()`` is the natural ceiling
  // (one slot per core).  Bound below at 4 so a 2-core CI host
  // still gets some parallelism; bound above at 32 so very wide
  // hosts don't immediately re-introduce the lock pathology.
  //
  // The semaphore is constructed exactly once (static local) — its
  // capacity is intentionally independent of the parent pool's
  // dispatch concurrency.
  static std::counting_semaphore<> shard_throttle {
      static_cast<std::ptrdiff_t>(
          std::clamp<unsigned>(std::thread::hardware_concurrency(), 4U, 32U)),
  };

  std::vector<std::jthread> tasks;
  tasks.reserve(path_tables.size());
  for (auto& [path, table] : path_tables) {
    tasks.emplace_back(
        [path = std::move(path), table = std::move(table), fmt, zfmt]
        {
          shard_throttle.acquire();
          struct ReleaseGuard
          {
            ReleaseGuard() = default;
            ReleaseGuard(const ReleaseGuard&) = delete;
            ReleaseGuard(ReleaseGuard&&) = delete;
            ReleaseGuard& operator=(const ReleaseGuard&) = delete;
            ReleaseGuard& operator=(ReleaseGuard&&) = delete;
            ~ReleaseGuard() { shard_throttle.release(); }
          } const guard {};
          SPDLOG_DEBUG("Writing table to '{}'", path.string());
          const auto st = write_table(fmt, path, table, zfmt);
          if (!st.ok()) {
            SPDLOG_CRITICAL(
                "File write failed for '{}': {}", path.string(), st.ToString());
          }
        });
  }
  for (auto&& t : tasks) {
    t.join();
  }
}

OutputContext::OutputContext(const SystemContext& psc,
                             LinearInterface& linear_interface,
                             SceneUid scene_uid,
                             PhaseUid phase_uid,
                             bool is_continuous_phase)
    : sc(psc)
    , m_scene_uid_(scene_uid)
    , m_phase_uid_(phase_uid)
    , m_is_continuous_phase_(is_continuous_phase)
    , m_output_selection_(psc.options().write_out())
    , col_sol_span(linear_interface.get_col_sol())
    , col_cost_span(linear_interface.get_col_cost())
    , row_dual_span(linear_interface.get_row_dual())
    , col_cost_scale_types(linear_interface.col_cost_scale_types())
    , row_cost_scale_types(linear_interface.row_cost_scale_types())
    , stb_prelude(make_stb_prelude(psc.stb_uids()))
    , st_prelude(make_st_prelude(psc.st_uids()))
    , t_prelude(make_t_prelude(psc.t_uids()))
{
  // Pre-reserve buckets for field_vector_map: a typical cell emits
  // ~150 unique (class, fname, sname) keys on juan/iplp (20+ element
  // classes × ~7 fields/class).  Reserving up-front avoids ~4-5
  // incremental rehashes on the per-cell insertions.  Kept as
  // `unordered_map` (not flat_map) because downstream write_out
  // iterates keys in any order.
  constexpr std::size_t map_reserve_size = 256;
  map_reserve(field_vector_map, map_reserve_size);
}

}  // namespace gtopt
