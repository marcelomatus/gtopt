/**
 * @file      input_traits.cpp
 * @brief     Input data access implementation
 * @date      Mon Mar 24 00:00:36 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements input data access using Arrow and Parquet
 */

#include <filesystem>
#include <format>
#include <expected>
#include <utility>

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/arrow_types.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/system_context.hpp>
#include <parquet/arrow/reader.h>
#include <spdlog/spdlog.h>

namespace {
using namespace gtopt;

[[nodiscard]] constexpr auto csv_read_table(const std::filesystem::path& fpath) 
    -> std::expected<ArrowTable, std::string>
{
    const auto filename = fpath.string() + ".csv";

    auto maybe_infile = arrow::io::ReadableFile::Open(filename);
    if (!maybe_infile.ok()) {
        return std::unexpected(std::format("Can't open file {}", filename));
    }

    const auto& infile = *maybe_infile;
    const auto& io_context = arrow::io::default_io_context();

    auto read_options = arrow::csv::ReadOptions::Defaults();
    read_options.autogenerate_column_names = false;

    auto parse_options = arrow::csv::ParseOptions::Defaults();

    auto convert_options = arrow::csv::ConvertOptions::Defaults();
    convert_options.column_types[std::string{Scenario::class_name}] = ArrowTraits<Uid>::type();
    convert_options.column_types[std::string{Stage::class_name}] = ArrowTraits<Uid>::type();
    convert_options.column_types[std::string{Block::class_name}] = ArrowTraits<Uid>::type();
    convert_options.include_missing_columns = true;

    auto maybe_reader = arrow::csv::TableReader::Make(
        io_context, infile, read_options, parse_options, convert_options);
    if (!maybe_reader.ok()) {
        return std::unexpected(std::format("Can't create CSV reader for {}", filename));
    }

    auto maybe_table = (*maybe_reader)->Read();
    if (!maybe_table.ok()) {
        return std::unexpected(std::format("Can't read CSV table from {}", filename));
    }

    SPDLOG_TRACE("Read table from file {}", filename);
    return *maybe_table;
}

[[nodiscard]] constexpr auto parquet_read_table(const std::filesystem::path& fpath) 
    -> std::expected<ArrowTable, std::string>
{
    const auto filename = fpath.string() + ".parquet";

    std::shared_ptr<arrow::io::RandomAccessFile> input;
    GTOPT_ARROW_ASSIGN_OR_RAISE(
        input,
        arrow::io::ReadableFile::Open(filename),
        std::format("Arrow can't open file {}", filename));

    auto* pool = arrow::default_memory_pool();
    std::unique_ptr<parquet::arrow::FileReader> reader;
    GTOPT_ARROW_ASSIGN_OR_RAISE(
        reader,
        parquet::arrow::OpenFile(input, pool),
        std::format("Parquet can't read file {}", filename));

    ArrowTable table;
    const arrow::Status st = reader->ReadTable(&table);
    if (!st.ok()) {
        return std::unexpected(std::format("Can't read Parquet table from {}", filename));
    }

    SPDLOG_TRACE("Read table from file {}", filename);
    return table;
}

} // namespace

namespace gtopt {

template<>
[[nodiscard]] ArrowTable InputTraits::arrow_read_table(const SystemContext& sc,
                                                     std::string_view cname,
                                                     std::string_view fname)
{
    auto fpath = std::filesystem::path(sc.options().input_directory());

    if (const auto pos = fname.find('@'); pos != std::string_view::npos) {
        fpath /= fname.substr(0, pos); // class name
        fpath /= fname.substr(pos + 1); // field name
    } else {
        fpath /= cname;
        fpath /= fname;
    }

    const auto try_read = [&fpath](std::string_view format) -> std::expected<ArrowTable, std::string> {
        if (format == "parquet") {
            if (auto table = parquet_read_table(fpath); table) {
                return table;
            }
            return csv_read_table(fpath);
        }
        
        if (auto table = csv_read_table(fpath); table) {
            return table;
        }
        return parquet_read_table(fpath);
    };

    const auto result = try_read(sc.options().input_format());
    if (!result) {
        const auto msg = std::format("Can't read table for class '{}' field '{}': {}",
                                    cname, fname, result.error());
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
    }

    SPDLOG_TRACE("Successfully loaded table for class {} field {}", cname, fname);
    return *result;
}

} // namespace gtopt
