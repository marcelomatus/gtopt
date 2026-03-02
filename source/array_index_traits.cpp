/**
 * @file      array_index_traits.cpp
 * @brief     Input data access implementation
 * @date      Mon Mar 24 00:00:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements input data access using Arrow and Parquet
 */

#include <cstddef>
#include <expected>
#include <filesystem>

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <arrow/io/compressed.h>
#include <arrow/util/compression.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/arrow_types.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/system_context.hpp>
#include <parquet/arrow/reader.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// Try to open a gzip-compressed file and return a decompressed InputStream.
[[nodiscard]] auto open_gzip_stream(const std::string& gz_filename)
    -> arrow::Result<std::shared_ptr<arrow::io::InputStream>>
{
  ARROW_ASSIGN_OR_RAISE(auto raw_file,
                        arrow::io::ReadableFile::Open(gz_filename));
  ARROW_ASSIGN_OR_RAISE(auto codec,
                        arrow::util::Codec::Create(arrow::Compression::GZIP));
  return arrow::io::CompressedInputStream::Make(codec.get(), raw_file);
}

// Decompress a gzip file into a random-access buffer (needed for Parquet).
[[nodiscard]] auto decompress_gzip_to_buffer(const std::string& gz_filename)
    -> arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>>
{
  ARROW_ASSIGN_OR_RAISE(auto stream, open_gzip_stream(gz_filename));

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

  // Try plain .csv first
  SPDLOG_DEBUG("csv_read_table: opening file '{}'", filename);
  auto maybe_infile = arrow::io::ReadableFile::Open(filename);
  if (maybe_infile.ok()) {
    SPDLOG_DEBUG("csv_read_table: creating CSV reader for '{}'", filename);
    auto maybe_table = [&]() -> arrow::Result<ArrowTable>
    {
      ARROW_ASSIGN_OR_RAISE(auto reader,
                            arrow::csv::TableReader::Make(io_context,
                                                          *maybe_infile,
                                                          read_options,
                                                          parse_options,
                                                          convert_options));
      return reader->Read();
    }();
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

  // Try .csv.gz as fallback
  SPDLOG_DEBUG("csv_read_table: trying gzip file '{}'", gz_filename);
  auto maybe_gz_stream = open_gzip_stream(gz_filename);
  if (!maybe_gz_stream.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to open '{}': {}",
                 gz_filename,
                 maybe_gz_stream.status().ToString());
    return std::unexpected(
        std::format("Can't open file {} or {}", filename, gz_filename));
  }

  auto maybe_table = [&]() -> arrow::Result<ArrowTable>
  {
    ARROW_ASSIGN_OR_RAISE(auto reader,
                          arrow::csv::TableReader::Make(io_context,
                                                        *maybe_gz_stream,
                                                        read_options,
                                                        parse_options,
                                                        convert_options));
    return reader->Read();
  }();
  if (!maybe_table.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to read '{}': {}",
                 gz_filename,
                 maybe_table.status().ToString());
    return std::unexpected(std::format("Can't read CSV table from {}: {}",
                                       gz_filename,
                                       maybe_table.status().message()));
  }

  SPDLOG_DEBUG("csv_read_table: successfully read '{}' ({} rows, {} cols)",
               gz_filename,
               (*maybe_table)->num_rows(),
               (*maybe_table)->num_columns());
  return *maybe_table;
}

[[nodiscard]] auto parquet_read_table(const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>
{
  const auto filename = std::format("{}.parquet", fpath.string());
  const auto gz_filename = std::format("{}.parquet.gz", fpath.string());

  auto* pool = arrow::default_memory_pool();

  // Try plain .parquet first
  SPDLOG_DEBUG("parquet_read_table: opening file '{}'", filename);
  std::shared_ptr<arrow::io::RandomAccessFile> input;
  {
    auto&& ofile = arrow::io::ReadableFile::Open(filename);
    if (ofile.ok()) {
      input = std::move(ofile).ValueUnsafe();
    }
  }

  if (input) {
    std::unique_ptr<parquet::arrow::FileReader> reader;
    {
      SPDLOG_DEBUG("parquet_read_table: creating Parquet reader for '{}'",
                   filename);
#if ARROW_VERSION_MAJOR >= 19
      auto&& ofile = parquet::arrow::OpenFile(input, pool);
      if (ofile.ok()) {
        reader = std::move(ofile).ValueUnsafe();
      }
#else
      auto st = parquet::arrow::OpenFile(input, pool, &reader);
      if (!st.ok()) {
        reader.reset();
      }
#endif
    }

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
        return table;
      }
      SPDLOG_DEBUG("parquet_read_table: failed to read table from '{}': {}",
                   filename,
                   st.ToString());
    }
  } else {
    SPDLOG_DEBUG("parquet_read_table: failed to open '{}'", filename);
  }

  // Try .parquet.gz as fallback
  SPDLOG_DEBUG("parquet_read_table: trying gzip file '{}'", gz_filename);
  auto maybe_buffer = decompress_gzip_to_buffer(gz_filename);
  if (!maybe_buffer.ok()) {
    SPDLOG_DEBUG("parquet_read_table: failed to open '{}': {}",
                 gz_filename,
                 maybe_buffer.status().ToString());
    return std::unexpected(
        std::format("Arrow can't open file {} or {}", filename, gz_filename));
  }

  std::unique_ptr<parquet::arrow::FileReader> reader;
  {
#if ARROW_VERSION_MAJOR >= 19
    auto&& ofile = parquet::arrow::OpenFile(*maybe_buffer, pool);
    if (!ofile.ok()) {
      return std::unexpected(
          std::format("Arrow can't open file {}", gz_filename));
    }
    reader = std::move(ofile).ValueUnsafe();
#else
    auto st = parquet::arrow::OpenFile(*maybe_buffer, pool, &reader);
    if (!st.ok()) {
      return std::unexpected(
          std::format("Arrow can't open file {}", gz_filename));
    }
#endif
  }

  ArrowTable table;
  const arrow::Status st = reader->ReadTable(&table);
  if (!st.ok()) {
    return std::unexpected(
        std::format("Can't read Parquet table from {}", gz_filename));
  }

  SPDLOG_DEBUG("parquet_read_table: successfully read '{}' ({} rows, {} cols)",
               gz_filename,
               table->num_rows(),
               table->num_columns());
  return table;
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
  return *result;
}

}  // namespace gtopt
