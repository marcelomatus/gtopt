/**
 * @file      array_index_traits.cpp
 * @brief     Input data access implementation
 * @date      Mon Mar 24 00:00:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements input data access using Arrow and Parquet
 */

#include <array>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <arrow/io/compressed.h>
#include <arrow/util/compression.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/arrow_input_guards.hpp>
#include <gtopt/arrow_types.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/system_context.hpp>
#include <parquet/arrow/reader.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// Open a compressed file and return a decompressed InputStream.
[[nodiscard]] auto open_compressed_stream(const std::string& filename,
                                          arrow::Compression::type codec_type)
    -> arrow::Result<std::shared_ptr<arrow::io::InputStream>>
{
  ARROW_ASSIGN_OR_RAISE(auto raw_file, arrow::io::ReadableFile::Open(filename));
  ARROW_ASSIGN_OR_RAISE(auto codec, arrow::util::Codec::Create(codec_type));
  return arrow::io::CompressedInputStream::Make(codec.get(), raw_file);
}

// Decompress a compressed file into a random-access buffer (needed for
// Parquet).
[[nodiscard]] auto decompress_to_buffer(const std::string& filename,
                                        arrow::Compression::type codec_type)
    -> arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>>
{
  ARROW_ASSIGN_OR_RAISE(auto stream,
                        open_compressed_stream(filename, codec_type));

  // Read compressed stream in chunks into a buffer builder
  arrow::BufferBuilder builder;
  constexpr auto chunk_size = static_cast<int64_t>(1024 * 1024);  // 1 MB chunks
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto chunk, stream->Read(chunk_size));
    if (chunk->size() == 0) {
      break;
    }
    ARROW_RETURN_NOT_OK(builder.Append(chunk->data(), chunk->size()));
  }
  ARROW_ASSIGN_OR_RAISE(auto buffer, builder.Finish());
  return std::make_shared<arrow::io::BufferReader>(buffer);
}

}  // namespace

[[nodiscard]] auto csv_read_table(const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>
{
  const auto filename = std::format("{}.csv", fpath.string());
  const auto gz_filename = std::format("{}.csv.gz", fpath.string());
  const auto zst_filename = std::format("{}.csv.zst", fpath.string());

  auto read_options = arrow::csv::ReadOptions::Defaults();
  read_options.autogenerate_column_names = false;

  auto parse_options = arrow::csv::ParseOptions::Defaults();

  auto convert_options = arrow::csv::ConvertOptions::Defaults();
  convert_options.column_types[std::string {Scenario::class_name}] =
      ArrowTraits<Uid>::type();
  convert_options.column_types[std::string {Stage::class_name}] =
      ArrowTraits<Uid>::type();
  convert_options.column_types[std::string {Block::class_name}] =
      ArrowTraits<Uid>::type();
  convert_options.include_missing_columns = true;

  const auto& io_context = arrow::io::default_io_context();

  // Helper: try to read a CSV table from an open InputStream.  The
  // ``reject_non_numeric_columns`` guard fires AFTER a successful Read
  // — Arrow's CSV type-inference silently falls back to ``String`` on
  // any non-numeric token (e.g. ``"1, foo, 3"``), so we surface that
  // as a hard failure instead of carrying a string column into the
  // numeric LP build.  Integer values (``"1, -5"``) are accepted: the
  // guard only rejects non-{integer, floating} columns.
  const auto try_read_csv =
      [&](std::shared_ptr<arrow::io::InputStream> stream,
          std::string_view source_label) -> arrow::Result<ArrowTable>
  {
    ARROW_ASSIGN_OR_RAISE(auto reader,
                          arrow::csv::TableReader::Make(io_context,
                                                        std::move(stream),
                                                        read_options,
                                                        parse_options,
                                                        convert_options));
    ARROW_ASSIGN_OR_RAISE(auto table, reader->Read());
    reject_non_numeric_columns(*table, source_label);
    return table;
  };

  // Try plain .csv first
  SPDLOG_DEBUG("csv_read_table: opening file '{}'", filename);
  auto maybe_infile = arrow::io::ReadableFile::Open(filename);
  if (maybe_infile.ok()) {
    SPDLOG_DEBUG("csv_read_table: creating CSV reader for '{}'", filename);
    auto maybe_table = try_read_csv(*maybe_infile, filename);
    if (maybe_table.ok()) {
      SPDLOG_DEBUG("csv_read_table: successfully read '{}' ({} rows, {} cols)",
                   filename,
                   (*maybe_table)->num_rows(),
                   (*maybe_table)->num_columns());
      return *maybe_table;
    }
    SPDLOG_DEBUG("csv_read_table: failed to read '{}': {}",
                 filename,
                 maybe_table.status().ToString());
  } else {
    SPDLOG_DEBUG("csv_read_table: failed to open '{}': {}",
                 filename,
                 maybe_infile.status().ToString());
  }

  // Try .csv.gz as first fallback
  SPDLOG_DEBUG("csv_read_table: trying gzip file '{}'", gz_filename);
  auto maybe_gz_stream =
      open_compressed_stream(gz_filename, arrow::Compression::GZIP);
  if (maybe_gz_stream.ok()) {
    auto maybe_table = try_read_csv(*maybe_gz_stream, gz_filename);
    if (maybe_table.ok()) {
      SPDLOG_DEBUG("csv_read_table: successfully read '{}' ({} rows, {} cols)",
                   gz_filename,
                   (*maybe_table)->num_rows(),
                   (*maybe_table)->num_columns());
      return *maybe_table;
    }
    SPDLOG_DEBUG("csv_read_table: failed to read '{}': {}",
                 gz_filename,
                 maybe_table.status().ToString());
    return std::unexpected(std::format("Can't read CSV table from {}: {}",
                                       gz_filename,
                                       maybe_table.status().message()));
  }
  SPDLOG_DEBUG("csv_read_table: failed to open '{}': {}",
               gz_filename,
               maybe_gz_stream.status().ToString());

  // Try .csv.zst as second fallback (zstd remains a popular explicit
  // override of the default `snappy` Parquet codec for archival ratio,
  // and legacy outputs landed there before the default flip).
  SPDLOG_DEBUG("csv_read_table: trying zstd file '{}'", zst_filename);
  auto maybe_zst_stream =
      open_compressed_stream(zst_filename, arrow::Compression::ZSTD);
  if (!maybe_zst_stream.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to open '{}': {}",
                 zst_filename,
                 maybe_zst_stream.status().ToString());
    return std::unexpected(std::format(
        "Can't open file {}, {} or {}", filename, gz_filename, zst_filename));
  }

  auto maybe_table = try_read_csv(*maybe_zst_stream, zst_filename);
  if (!maybe_table.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to read '{}': {}",
                 zst_filename,
                 maybe_table.status().ToString());
    return std::unexpected(std::format("Can't read CSV table from {}: {}",
                                       zst_filename,
                                       maybe_table.status().message()));
  }

  SPDLOG_DEBUG("csv_read_table: successfully read '{}' ({} rows, {} cols)",
               zst_filename,
               (*maybe_table)->num_rows(),
               (*maybe_table)->num_columns());
  return *maybe_table;
}

[[nodiscard]] auto parquet_read_table(const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>
{
  const auto filename = std::format("{}.parquet", fpath.string());
  const auto gz_filename = std::format("{}.parquet.gz", fpath.string());
  const auto zst_filename = std::format("{}.parquet.zst", fpath.string());

  auto* pool = arrow::default_memory_pool();

  // Helper: open a Parquet reader from a random-access file buffer.
  const auto try_open_parquet =
      [&](std::shared_ptr<arrow::io::RandomAccessFile> buf,
          const std::string& path) -> std::expected<ArrowTable, std::string>
  {
    std::unique_ptr<parquet::arrow::FileReader> reader;
#if ARROW_VERSION_MAJOR >= 19
    auto&& ofile = parquet::arrow::OpenFile(std::move(buf), pool);
    if (!ofile.ok()) {
      return std::unexpected(std::format("Arrow can't open file {}", path));
    }
    reader = std::move(ofile).ValueUnsafe();
#else
    auto st = parquet::arrow::OpenFile(std::move(buf), pool, &reader);
    if (!st.ok()) {
      return std::unexpected(std::format("Arrow can't open file {}", path));
    }
#endif
    ArrowTable table;
    const arrow::Status st = reader->ReadTable(&table);
    if (!st.ok()) {
      return std::unexpected(
          std::format("Can't read Parquet table from {}", path));
    }
    SPDLOG_DEBUG(
        "parquet_read_table: successfully read '{}' ({} rows, {} cols)",
        path,
        table->num_rows(),
        table->num_columns());
    reject_nan_in_float_columns(*table, path);
    return table;
  };

  // Try plain .parquet first
  SPDLOG_DEBUG("parquet_read_table: opening file '{}'", filename);
  {
    auto&& ofile = arrow::io::ReadableFile::Open(filename);
    if (ofile.ok()) {
      const std::shared_ptr<arrow::io::RandomAccessFile> input =
          std::move(ofile).ValueUnsafe();
      std::unique_ptr<parquet::arrow::FileReader> reader;
      SPDLOG_DEBUG("parquet_read_table: creating Parquet reader for '{}'",
                   filename);
#if ARROW_VERSION_MAJOR >= 19
      auto&& oreader = parquet::arrow::OpenFile(input, pool);
      if (oreader.ok()) {
        reader = std::move(oreader).ValueUnsafe();
      }
#else
      auto st = parquet::arrow::OpenFile(input, pool, &reader);
      if (!st.ok()) {
        reader.reset();
      }
#endif
      if (reader) {
        SPDLOG_DEBUG("parquet_read_table: reading table from '{}'", filename);
        ArrowTable table;
        const arrow::Status st = reader->ReadTable(&table);
        if (st.ok()) {
          SPDLOG_DEBUG(
              "parquet_read_table: successfully read '{}' ({} rows, {} cols)",
              filename,
              table->num_rows(),
              table->num_columns());
          reject_nan_in_float_columns(*table, filename);
          return table;
        }
        SPDLOG_DEBUG("parquet_read_table: failed to read table from '{}': {}",
                     filename,
                     st.ToString());
      }
    } else {
      SPDLOG_DEBUG("parquet_read_table: failed to open '{}'", filename);
    }
  }

  // Try .parquet.gz as first fallback
  SPDLOG_DEBUG("parquet_read_table: trying gzip file '{}'", gz_filename);
  {
    auto maybe_buffer =
        decompress_to_buffer(gz_filename, arrow::Compression::GZIP);
    if (maybe_buffer.ok()) {
      return try_open_parquet(*maybe_buffer, gz_filename);
    }
    SPDLOG_DEBUG("parquet_read_table: failed to open '{}': {}",
                 gz_filename,
                 maybe_buffer.status().ToString());
  }

  // Try .parquet.zst as second fallback
  SPDLOG_DEBUG("parquet_read_table: trying zstd file '{}'", zst_filename);
  {
    auto maybe_buffer =
        decompress_to_buffer(zst_filename, arrow::Compression::ZSTD);
    if (maybe_buffer.ok()) {
      return try_open_parquet(*maybe_buffer, zst_filename);
    }
    SPDLOG_DEBUG("parquet_read_table: failed to open '{}': {}",
                 zst_filename,
                 maybe_buffer.status().ToString());
  }

  return std::unexpected(std::format("Arrow can't open file {}, {} or {}",
                                     filename,
                                     gz_filename,
                                     zst_filename));
}

[[nodiscard]] std::filesystem::path build_table_path(std::string_view input_dir,
                                                     std::string_view cname,
                                                     std::string_view fname)
{
  auto fpath = std::filesystem::path(input_dir);

  if (const auto pos = fname.find('@'); pos != std::string_view::npos) {
    fpath /= fname.substr(0, pos);  // override class directory
    fpath /= fname.substr(pos + 1);  // field name
  } else {
    fpath /= cname;
    fpath /= fname;
  }

  return fpath;
}

[[nodiscard]] auto try_read_table(const std::filesystem::path& fpath,
                                  std::string_view format)
    -> std::expected<ArrowTable, std::string>
{
  if (format == "parquet") {
    SPDLOG_DEBUG("try_read_table: trying parquet format first for '{}'",
                 fpath.string());
    auto table = parquet_read_table(fpath);
    if (table) {
      return table;
    }
    SPDLOG_WARN(
        "try_read_table: parquet failed for '{}', "
        "falling back to CSV: {}",
        fpath.string(),
        table.error());
    return csv_read_table(fpath);
  }

  SPDLOG_DEBUG("try_read_table: trying CSV format first for '{}'",
               fpath.string());
  auto table = csv_read_table(fpath);
  if (table) {
    return table;
  }
  SPDLOG_WARN(
      "try_read_table: CSV failed for '{}', "
      "falling back to parquet: {}",
      fpath.string(),
      table.error());
  return parquet_read_table(fpath);
}

namespace
{

// Append every value of `a` (downcast to the concrete unsigned ArrayT)
// to `builder`, widened to int32.  The downcast is guarded by the
// caller's type_id() switch, hence the single localized NOLINT.
template<typename ArrayT>
void append_uint_as_int32(arrow::Int32Builder& builder, const arrow::Array& a)
{
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  const auto& typed = static_cast<const ArrayT&>(a);
  for (int64_t i = 0; i < typed.length(); ++i) {
    if (typed.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(static_cast<int32_t>(typed.Value(i)));
    }
  }
}

// Widen any integer Arrow array (signed or unsigned, 8-64 bit) to Int32.
// `cast_to_int32_array` in arrow_types.hpp only covers signed ints; the
// long index/uid columns can also arrive as uint16 (gtopt's own long
// output) so we need the wider net here.
[[nodiscard]] auto widen_any_int_to_int32(
    const std::shared_ptr<arrow::Array>& a)
    -> std::shared_ptr<arrow::Int32Array>
{
  if (!a) {
    return nullptr;
  }
  if (auto signed_cast = cast_to_int32_array(a)) {
    return signed_cast;
  }
  arrow::Int32Builder builder;
  if (!builder.Reserve(a->length()).ok()) {
    return nullptr;
  }
  switch (a->type_id()) {
    case arrow::Type::UINT8:
      append_uint_as_int32<arrow::UInt8Array>(builder, *a);
      break;
    case arrow::Type::UINT16:
      append_uint_as_int32<arrow::UInt16Array>(builder, *a);
      break;
    case arrow::Type::UINT32:
      append_uint_as_int32<arrow::UInt32Array>(builder, *a);
      break;
    case arrow::Type::UINT64:
      append_uint_as_int32<arrow::UInt64Array>(builder, *a);
      break;
    default:
      return nullptr;
  }
  std::shared_ptr<arrow::Array> out;
  if (!builder.Finish(&out).ok()) {
    return nullptr;
  }
  return std::static_pointer_cast<arrow::Int32Array>(out);
}

// Assemble the wide table from pre-extracted long columns.  `CType` is the
// value C++ type (double or int32_t).  Index columns (≤ 3: a subset of
// scenario/stage/block) are packed into a fixed-size key so the pivot does
// no per-row heap allocation.
template<typename CType>
[[nodiscard]] auto assemble_wide(
    int64_t n_rows,
    const std::vector<std::string>& idx_names,
    const std::vector<std::shared_ptr<arrow::Int32Array>>& idx_arrs,
    const std::shared_ptr<arrow::Int32Array>& uid_arr,
    const std::vector<CType>& vals) -> ArrowTable
{
  const auto n_idx = idx_names.size();
  using key_t = std::array<int32_t, 3>;  // index cols ⊆ {scenario,stage,block}

  const auto row_key = [&](int64_t r) -> key_t
  {
    key_t key {};
    for (std::size_t j = 0; j < n_idx; ++j) {
      key.at(j) = idx_arrs[j]->IsNull(r) ? 0 : idx_arrs[j]->Value(r);
    }
    return key;
  };

  std::map<key_t, int64_t> key_row;
  std::vector<std::vector<int32_t>> idx_out(n_idx);
  std::map<int32_t, std::size_t> uid_col;
  std::vector<int32_t> uid_order;

  // Pass 1 — discover distinct keys (→ wide rows) and distinct uids (→ cols).
  for (int64_t r = 0; r < n_rows; ++r) {
    const auto [it, inserted] =
        key_row.try_emplace(row_key(r), static_cast<int64_t>(key_row.size()));
    if (inserted) {
      const auto key = it->first;
      for (std::size_t j = 0; j < n_idx; ++j) {
        idx_out[j].push_back(key.at(j));
      }
    }
    const int32_t u = uid_arr->IsNull(r) ? 0 : uid_arr->Value(r);
    if (uid_col.try_emplace(u, uid_order.size()).second) {
      uid_order.push_back(u);
    }
  }

  const auto n_keys = static_cast<int64_t>(key_row.size());
  std::vector<std::vector<CType>> val_out(
      uid_order.size(), std::vector<CType>(n_keys, CType {0}));

  // Pass 2 — scatter values into the (uid-col, key-row) grid.
  for (int64_t r = 0; r < n_rows; ++r) {
    const int64_t row = key_row.at(row_key(r));
    const int32_t u = uid_arr->IsNull(r) ? 0 : uid_arr->Value(r);
    val_out[uid_col.at(u)][static_cast<std::size_t>(row)] = vals[r];
  }

  std::vector<ArrowField> fields;
  std::vector<ArrowArray> arrays;
  fields.reserve(n_idx + uid_order.size());
  arrays.reserve(n_idx + uid_order.size());

  const auto finish = [](auto& builder) -> ArrowArray
  {
    ArrowArray out;
    if (!builder.Finish(&out).ok()) {
      throw std::runtime_error("pivot_long_to_wide: array build failed");
    }
    return out;
  };

  for (std::size_t j = 0; j < n_idx; ++j) {
    arrow::Int32Builder builder;
    if (!builder.AppendValues(idx_out[j]).ok()) {
      throw std::runtime_error("pivot_long_to_wide: index build failed");
    }
    fields.push_back(arrow::field(idx_names[j], arrow::int32()));
    arrays.push_back(finish(builder));
  }
  for (std::size_t c = 0; c < uid_order.size(); ++c) {
    typename arrow::CTypeTraits<CType>::BuilderType builder;
    if (!builder.AppendValues(val_out[c]).ok()) {
      throw std::runtime_error("pivot_long_to_wide: value build failed");
    }
    fields.push_back(arrow::field("uid:" + std::to_string(uid_order[c]),
                                  arrow::CTypeTraits<CType>::type_singleton()));
    arrays.push_back(finish(builder));
  }

  return arrow::Table::Make(arrow::schema(std::move(fields)), arrays);
}

}  // namespace

[[nodiscard]] auto is_long_layout(const ArrowTable& table) -> bool
{
  return table && table->GetColumnByName("uid") != nullptr
      && table->GetColumnByName("value") != nullptr;
}

[[nodiscard]] auto pivot_long_to_wide(const ArrowTable& table_in) -> ArrowTable
{
  // Collapse chunks so every column has a single contiguous chunk.
  ArrowTable table = table_in;
  if (auto combined = table_in->CombineChunks(); combined.ok()) {
    table = *combined;
  }

  const int64_t n_rows = table->num_rows();
  // An empty long file is degenerate; hand it back untouched and let the
  // wide path raise its usual "can't find element" diagnostic.
  if (n_rows == 0) {
    return table_in;
  }

  const auto first_chunk = [](const ArrowChunkedArray& col)
  { return col->num_chunks() > 0 ? col->chunk(0) : nullptr; };

  auto uid_arr =
      widen_any_int_to_int32(first_chunk(table->GetColumnByName("uid")));
  if (!uid_arr) {
    throw std::runtime_error("pivot_long_to_wide: 'uid' column is not integer");
  }

  std::vector<std::string> idx_names;
  std::vector<std::shared_ptr<arrow::Int32Array>> idx_arrs;
  for (int c = 0; c < table->num_columns(); ++c) {
    const auto& name = table->schema()->field(c)->name();
    if (name == "uid" || name == "value") {
      continue;
    }
    auto arr = widen_any_int_to_int32(first_chunk(table->column(c)));
    if (!arr) {
      throw std::runtime_error(std::format(
          "pivot_long_to_wide: index column '{}' is not integer", name));
    }
    idx_names.push_back(name);
    idx_arrs.push_back(std::move(arr));
  }

  const auto value_col = table->GetColumnByName("value");
  const auto value_chunk = first_chunk(value_col);
  const auto vtid = value_col->type()->id();

  if (is_compatible_double_type(vtid)) {
    auto va = cast_to_double_array(value_chunk);
    std::vector<double> vals(static_cast<std::size_t>(n_rows));
    for (int64_t r = 0; r < n_rows; ++r) {
      vals[static_cast<std::size_t>(r)] = va->IsNull(r) ? 0.0 : va->Value(r);
    }
    return assemble_wide<double>(n_rows, idx_names, idx_arrs, uid_arr, vals);
  }

  if (auto va = widen_any_int_to_int32(value_chunk)) {
    std::vector<int32_t> vals(static_cast<std::size_t>(n_rows));
    for (int64_t r = 0; r < n_rows; ++r) {
      vals[static_cast<std::size_t>(r)] = va->IsNull(r) ? 0 : va->Value(r);
    }
    return assemble_wide<int32_t>(n_rows, idx_names, idx_arrs, uid_arr, vals);
  }

  throw std::runtime_error(std::format(
      "pivot_long_to_wide: 'value' column has unsupported type '{}'",
      value_col->type()->ToString()));
}

[[nodiscard]] ArrowTable ArrayIndexBase::read_arrow_table(
    const SystemContext& sc, std::string_view cname, std::string_view fname)
{
  const auto fpath =
      build_table_path(sc.options().input_directory(), cname, fname);

  SPDLOG_DEBUG("Reading input table: class='{}' field='{}' path='{}'",
               cname,
               fname,
               fpath.string());

  const auto result = try_read_table(fpath, sc.options().input_format());
  if (!result) {
    const auto msg =
        std::format("Can't read table for class '{}' field '{}': {}",
                    cname,
                    fname,
                    result.error());
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  SPDLOG_DEBUG("Loaded table for class='{}' field='{}': {} rows, {} columns",
               cname,
               fname,
               (*result)->num_rows(),
               (*result)->num_columns());

  // Return the raw table unchanged.  get_arrow_index now owns the long/wide
  // decision: long input is indexed directly (per-uid submaps over the long
  // rows) for the (Scenario, Stage, Block) reader, and only pivoted to wide
  // as a fallback for arities without a long-direct index builder.
  return *result;
}

}  // namespace gtopt
