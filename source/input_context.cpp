/**
 * @file      input_context.cpp
 * @brief     Header of
 * @date      Mon Mar 24 00:00:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <filesystem>
#include <format>

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/input_context.hpp>
#include <parquet/arrow/reader.h>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

using ArrowTable = std::shared_ptr<arrow::Table>;

inline ArrowTable csv_read_table(auto&& fpath)
{
  const auto filename = fpath.string() + ".csv";

  auto maybe_infile = arrow::io::ReadableFile::Open(filename);
  if (!maybe_infile.ok()) {
    SPDLOG_WARN("can't open file {}", filename);
    return nullptr;
  }
  const auto& infile = *maybe_infile;

  const arrow::io::IOContext& io_context = arrow::io::default_io_context();
  auto read_options = arrow::csv::ReadOptions::Defaults();

  read_options.autogenerate_column_names = false;

  auto parse_options = arrow::csv::ParseOptions::Defaults();

  const std::string scenario_str {Scenario::class_name};
  const std::string stage_str {Stage::class_name};
  const std::string block_str {Block::class_name};

  auto convert_options = arrow::csv::ConvertOptions::Defaults();
  convert_options.column_types[scenario_str] = ArrowTraits<Uid>::type();
  convert_options.column_types[stage_str] = ArrowTraits<Uid>::type();
  convert_options.column_types[block_str] = ArrowTraits<Uid>::type();

  convert_options.include_missing_columns = true;

  auto maybe_reader = arrow::csv::TableReader::Make(
      io_context, infile, read_options, parse_options, convert_options);
  if (!maybe_reader.ok()) {
    SPDLOG_WARN("can't read file {}", filename);
  }

  const auto& reader = *maybe_reader;

  auto maybe_table = reader->Read();
  if (!maybe_table.ok()) {
    SPDLOG_WARN("can't read table {}", filename);
    return {};
  }

  ArrowTable table = *maybe_table;
  SPDLOG_TRACE("read table from file {}", filename);

  return table;
}

inline ArrowTable parquet_read_table(auto&& fpath)
{
  const auto filename = fpath.string() + ".parquet";

  std::shared_ptr<arrow::io::RandomAccessFile> input;
  GTOPT_ARROW_ASSING_OR_RAISE(
      input,
      arrow::io::ReadableFile::Open(filename),
      std::format("arrow can't open file {}", filename));

  auto* pool = arrow::default_memory_pool();
  std::unique_ptr<parquet::arrow::FileReader> reader;
  GTOPT_ARROW_ASSING_OR_RAISE(
      reader,
      parquet::arrow::OpenFile(input, pool),
      std::format("parquet can't read file {}", filename));

  ArrowTable table;
  const arrow::Status st = reader->ReadTable(&table);
  if (!st.ok()) {
    SPDLOG_WARN("can't read table {}", filename);
    return {};
  }

  SPDLOG_TRACE("read table from file {}", filename);

  return table;
}

}  // namespace

namespace gtopt
{
using ArrowTable = std::shared_ptr<arrow::Table>;

template<>
ArrowTable InputTraits::read_table(const SystemContext& sc,
                                   const std::string_view& cname,
                                   const std::string_view& fname)
{
  auto fpath = std::filesystem::path(sc.options().input_directory());

  const auto pos = fname.find('@');
  if (pos != std::string::npos) {
    fpath /= fname.substr(0, pos);  // class name
    fpath /= fname.substr(pos + 1, fname.size());  // field name
  } else {
    fpath /= cname;
    fpath /= fname;
  }

  ArrowTable table;
  if (sc.options().input_format() == "parquet") {
    table = parquet_read_table(fpath);
    if (!table) {
      SPDLOG_WARN("can't read parquet file {}, trying to read csv file",
                  fpath.string());

      table = csv_read_table(fpath);
    }
  } else {
    table = csv_read_table(fpath);
    if (!table) {
      SPDLOG_WARN("can't read csv file {}, trying to read parquet file",
                  fpath.string());

      table = parquet_read_table(fpath);
    }
  }

  if (!table) {
    const auto msg = std::format(
        "can't read table with class '{}' and field '{}'", cname, fname);
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  SPDLOG_TRACE(
      "successfully loading table for class {} and field {}", cname, fname);
  return table;
}

}  // namespace gtopt
