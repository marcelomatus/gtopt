/**
 * @file      input_traits.cpp
 * @brief     Input data access implementation
 * @date      Mon Mar 24 00:00:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements input data access using Arrow and Parquet
 */

#include <expected>
#include <filesystem>

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/arrow_types.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/system_context.hpp>
#include <parquet/arrow/reader.h>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

[[nodiscard]] constexpr auto csv_read_table(const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>
{
  const auto filename = std::format("{}.csv", fpath.string());

  SPDLOG_DEBUG("csv_read_table: opening file '{}'", filename);
  auto maybe_infile = arrow::io::ReadableFile::Open(filename);
  if (!maybe_infile.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to open '{}': {}",
                 filename,
                 maybe_infile.status().ToString());
    return std::unexpected(std::format("Can't open file {}", filename));
  }

  const auto& infile = *maybe_infile;
  const auto& io_context = arrow::io::default_io_context();

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

  SPDLOG_DEBUG("csv_read_table: creating CSV reader for '{}'", filename);
  auto maybe_reader = arrow::csv::TableReader::Make(
      io_context, infile, read_options, parse_options, convert_options);
  if (!maybe_reader.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to create reader for '{}': {}",
                 filename,
                 maybe_reader.status().ToString());
    return std::unexpected(
        std::format("Can't create CSV reader for {}", filename));
  }

  SPDLOG_DEBUG("csv_read_table: reading table from '{}'", filename);
  auto maybe_table = (*maybe_reader)->Read();
  if (!maybe_table.ok()) {
    SPDLOG_DEBUG("csv_read_table: failed to read table from '{}': {}",
                 filename,
                 maybe_table.status().ToString());
    return std::unexpected(
        std::format("Can't read CSV table from {}", filename));
  }

  SPDLOG_DEBUG("csv_read_table: successfully read '{}' ({} rows, {} cols)",
               filename,
               (*maybe_table)->num_rows(),
               (*maybe_table)->num_columns());
  return *maybe_table;
}

[[nodiscard]] constexpr auto parquet_read_table(
    const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>
{
  const auto filename = std::format("{}.parquet", fpath.string());

  SPDLOG_DEBUG("parquet_read_table: opening file '{}'", filename);
  std::shared_ptr<arrow::io::RandomAccessFile> input;
  {
    auto&& ofile = arrow::io::ReadableFile::Open(filename);
    {
      if (not(ofile.ok())) {
        SPDLOG_DEBUG("parquet_read_table: failed to open '{}': {}",
                     filename,
                     ofile.status().ToString());
        return std::unexpected(
            std::format("Arrow can't open file {}", filename));
      }
    }
    input = std::move(ofile).ValueUnsafe();
  }

  auto* pool = arrow::default_memory_pool();
  std::unique_ptr<parquet::arrow::FileReader> reader;
  {
    SPDLOG_DEBUG("parquet_read_table: creating Parquet reader for '{}'",
                 filename);
    auto&& ofile = parquet::arrow::OpenFile(input, pool);
    {
      if (not(ofile.ok())) {
        SPDLOG_DEBUG(
            "parquet_read_table: failed to create Parquet reader for '{}': {}",
            filename,
            ofile.status().ToString());
        return std::unexpected(
            std::format("Arrow can't open file {}", filename));
      }
    }
    reader = std::move(ofile).ValueUnsafe();
  }

  SPDLOG_DEBUG("parquet_read_table: reading table from '{}'", filename);
  ArrowTable table;
  const arrow::Status st = reader->ReadTable(&table);
  if (!st.ok()) {
    SPDLOG_DEBUG("parquet_read_table: failed to read table from '{}': {}",
                 filename,
                 st.ToString());
    return std::unexpected(
        std::format("Can't read Parquet table from {}", filename));
  }

  SPDLOG_DEBUG("parquet_read_table: successfully read '{}' ({} rows, {} cols)",
               filename,
               table->num_rows(),
               table->num_columns());
  return table;
}

}  // namespace

namespace gtopt
{

[[nodiscard]] ArrowTable ArrayIndexBase::read_arrow_table(
    const SystemContext& sc, std::string_view cname, std::string_view fname)
{
  auto fpath = std::filesystem::path(sc.options().input_directory());

  if (const auto pos = fname.find('@'); pos != std::string_view::npos) {
    fpath /= fname.substr(0, pos);  // class name
    fpath /= fname.substr(pos + 1);  // field name
  } else {
    fpath /= cname;
    fpath /= fname;
  }

  SPDLOG_DEBUG("Reading input table: class='{}' field='{}' path='{}'",
               cname,
               fname,
               fpath.string());

  const auto try_read = [&fpath](std::string_view format)
      -> std::expected<ArrowTable, std::string>
  {
    if (format == "parquet") {
      SPDLOG_DEBUG("read_arrow_table: trying parquet format first for '{}'",
                   fpath.string());
      auto table = parquet_read_table(fpath);
      if (table) {
        return table;
      }
      SPDLOG_WARN("read_arrow_table: parquet failed for '{}', "
                  "falling back to CSV: {}",
                  fpath.string(),
                  table.error());
      return csv_read_table(fpath);
    }

    SPDLOG_DEBUG("read_arrow_table: trying CSV format first for '{}'",
                 fpath.string());
    auto table = csv_read_table(fpath);
    if (table) {
      return table;
    }
    SPDLOG_WARN("read_arrow_table: CSV failed for '{}', "
                "falling back to parquet: {}",
                fpath.string(),
                table.error());
    return parquet_read_table(fpath);
  };

  const auto result = try_read(sc.options().input_format());
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
