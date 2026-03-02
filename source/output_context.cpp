/**
 * @file      output_context.cpp
 * @brief     Header of
 * @date      Wed Mar 26 01:05:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <algorithm>  // For std::find
#include <concepts>
#include <filesystem>
#include <fstream>

#include <arrow/api.h>  // Add missing arrow headers
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <arrow/io/compressed.h>
#include <arrow/util/compression.h>
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
template<typename Type = double, typename FieldVector>
auto make_field_arrays(FieldVector&& field_vector)
{
  std::vector<ArrowField> fields;
  fields.reserve(field_vector.size() + 3);
  std::vector<ArrowArray> arrays;
  arrays.reserve(field_vector.size() + 3);

  bool prelude_processed = false;

  for (auto&& field_data : std::forward<FieldVector>(field_vector)) {
    auto&& [fname, fvalues, fvalids, prelude] =
        std::forward<decltype(field_data)>(field_data);

    if (fvalues.empty()) [[unlikely]] {
      continue;
    }

    // Process prelude only once
    if (!prelude_processed && prelude) [[likely]] {
      auto&& [pfields, parrays] = *std::forward<decltype(prelude)>(prelude);

      // Move if rvalue, copy if lvalue
      if constexpr (std::is_rvalue_reference_v<FieldVector&&>) {
        fields = std::move(pfields);
        arrays = std::move(parrays);
      } else {
        fields = pfields;
        arrays = parrays;
      }
      prelude_processed = true;
    }

    fields.emplace_back(arrow::field(std::forward<decltype(fname)>(fname),
                                     ArrowTraits<Type>::type()));

    arrays.emplace_back(
        make_array<Type>(std::forward<decltype(fvalues)>(fvalues),
                         std::forward<decltype(fvalids)>(fvalids)));
  }

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = double, typename FieldVector>
auto make_table(FieldVector&& field_vector)
    -> arrow::Result<std::shared_ptr<arrow::Table>>
{
  auto [fields, arrays] =
      make_field_arrays<Type>(std::forward<FieldVector>(field_vector));

  return arrow::Table::Make(arrow::schema(std::move(fields)),
                            std::move(arrays));
}

// Resolve a Parquet compression codec from a string name, checking runtime
// library support and falling back gracefully with warnings.
//
// Resolution order:
//   1. Empty / "none" / "uncompressed" → UNCOMPRESSED (no check needed).
//   2. Known name but not supported at runtime   → warn, use GZIP.
//   3. Unknown name                              → warn, use GZIP.
[[nodiscard]] auto resolve_parquet_codec(const std::string& zfmt)
{
  using codec_t = decltype(parquet::Compression::UNCOMPRESSED);
  static const std::map<std::string, codec_t> codec_map = {
      // Empty string / explicit "none" / "uncompressed" → no compression.
      // NOTE: "" is a valid key here; get_optvalue returns a value (not
      // nullopt) for it, so it never reaches the fallback branches below.
      {"", parquet::Compression::UNCOMPRESSED},
      {"none", parquet::Compression::UNCOMPRESSED},
      {"uncompressed", parquet::Compression::UNCOMPRESSED},
      {"gzip", parquet::Compression::GZIP},
      {"zstd", parquet::Compression::ZSTD},
      {"lzo", parquet::Compression::LZO},
  };

  const auto desired_opt = get_optvalue(codec_map, zfmt);

  // Unknown name: not in the map and non-empty.
  if (!desired_opt.has_value() && !zfmt.empty()) {
    SPDLOG_WARN("Parquet compression '{}' is unknown; falling back to gzip",
                zfmt);
    return parquet::Compression::GZIP;
  }

  const codec_t codec =
      desired_opt.value_or(parquet::Compression::UNCOMPRESSED);

  // For uncompressed, no library check is needed.
  if (codec == parquet::Compression::UNCOMPRESSED) {
    return codec;
  }

  // Known but not supported at runtime → fall back to gzip.
  if (!parquet::IsCodecSupported(codec)) {
    SPDLOG_WARN(
        "Parquet compression '{}' is not supported by the linked Parquet "
        "library; falling back to gzip",
        zfmt);
    return parquet::Compression::GZIP;
  }

  return codec;
}

auto parquet_write_table(const auto& fpath,
                         const auto& table,
                         const auto& zfmt) -> arrow::Status
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
  const auto props = props_builder.build();

  auto status = parquet::arrow::WriteTable(*table.get(),
                                           arrow::default_memory_pool(),
                                           output,
                                           1024 * 1024,  // NOLINT
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

auto csv_write_table_gzip(const auto& fpath, const auto& table)
{
  const auto filename = std::format("{}.csv.gz", fpath.string());
  ARROW_ASSIGN_OR_RAISE(auto file_output,
                        arrow::io::FileOutputStream::Open(filename));
  ARROW_ASSIGN_OR_RAISE(auto codec,
                        arrow::util::Codec::Create(arrow::Compression::GZIP));
  ARROW_ASSIGN_OR_RAISE(
      auto gzip_output,
      arrow::io::CompressedOutputStream::Make(codec.get(), file_output));

  const auto write_options = arrow::csv::WriteOptions::Defaults();
  ARROW_RETURN_NOT_OK(WriteCSV(*table.get(), write_options, gzip_output.get()));
  ARROW_RETURN_NOT_OK(gzip_output->Close());
  return file_output->Close();
}

auto csv_write_table(const auto& fpath, const auto& table, const auto& zfmt)
{
  // Default for CSV is no compression; only "gzip" is supported.
  // Any other non-empty format triggers a warning and falls back to gzip.
  if (zfmt.empty() || zfmt == "none" || zfmt == "uncompressed") {
    return csv_write_table_plain(fpath, table);
  }

  if (zfmt != "gzip") {
    SPDLOG_WARN("CSV compression '{}' is not supported; falling back to gzip",
                zfmt);
  }
  return csv_write_table_gzip(fpath, table);
}

auto write_table(std::string_view fmt,
                 const auto& fpath,
                 const auto& table,
                 const std::string& zfmt)
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
auto create_tables(auto&& output_directory, auto&& field_vector_map)
{
  using PathTable =
      std::pair<std::filesystem::path, std::shared_ptr<arrow::Table>>;

  std::vector<PathTable> path_tables;

  const auto dirpath = std::filesystem::path(output_directory);
  for (auto&& [class_fname, vfields] : field_vector_map) {
    auto&& [cname, fname] = class_fname;
    const auto mtable = make_table<Type>(vfields);

    const auto dpath = dirpath / cname;

    std::error_code ec;
    std::filesystem::create_directories(dpath, ec);
    if (ec) {
      SPDLOG_CRITICAL("Cannot create output directory '{}': {}",
                      dpath.string(),
                      ec.message());
      continue;
    }

    path_tables.emplace_back(dpath / fname, *mtable);
  }

  return path_tables;
}

}  // namespace

namespace gtopt
{

void OutputContext::write() const
{
  const auto fmt = options().output_format();
  const auto zfmt = options().output_compression();
  auto path_tables =
      create_tables(options().output_directory(), field_vector_map);

  spdlog::info(std::format(
      "  Writing {} output tables to '{}' (format={}, compression={})",
      path_tables.size(),
      options().output_directory(),
      fmt,
      zfmt));

  std::vector<std::jthread> tasks;
  tasks.reserve(path_tables.size());
  for (auto& [path, table] : path_tables) {
    tasks.emplace_back(
        [path = std::move(path), table = std::move(table), fmt, zfmt]
        {
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

  const auto sol_path =
      std::filesystem::path(options().output_directory()) / "solution.csv";

  spdlog::info(std::format("  Write solution to '{}' (status={}, obj_value={})",
                           sol_path.string(),
                           sol_status,
                           sol_obj_value));

  std::ofstream sol_file(sol_path.string());
  if (!sol_file) [[unlikely]] {
    SPDLOG_CRITICAL("Cannot open solution file '{}' for writing",
                    sol_path.string());
    return;
  }
  sol_file << std::format("{:>12},{}\n{:>12},{}\n{:>12},{}",
                          "obj_value",
                          sol_obj_value,
                          "kappa",
                          sol_kappa,
                          "status",
                          sol_status);
  sol_file << '\n';
}

OutputContext::OutputContext(const SystemContext& psc,
                             const LinearInterface& linear_interface)
    : sc(psc)
    , sol_obj_value(linear_interface.get_obj_value())
    , sol_status(linear_interface.get_status())
    , sol_kappa(linear_interface.get_kappa())
    , col_sol_span(linear_interface.get_col_sol())
    , col_cost_span(linear_interface.get_col_cost())
    , row_dual_span(linear_interface.get_row_dual())
    , block_cost_factors(psc.block_icost_factors())
    , stage_cost_factors(psc.stage_icost_factors())
    , scenario_stage_cost_factors(psc.scenario_stage_icost_factors())
    , stb_prelude(make_stb_prelude(psc.stb_uids()))
    , st_prelude(make_st_prelude(psc.st_uids()))
    , t_prelude(make_t_prelude(psc.t_uids()))
{
}

}  // namespace gtopt
