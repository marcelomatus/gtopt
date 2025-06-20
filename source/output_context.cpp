/**
 * @file      output_context.cpp
 * @brief     Header of
 * @date      Wed Mar 26 01:05:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <filesystem>
#include <concepts>
#include <ranges>

template<typename T>
concept ArrowBuildable = requires {
  { arrow::CTypeTraits<T>::type_singleton() } -> std::same_as<std::shared_ptr<arrow::DataType>>;
};

constexpr void validate_output_format(std::string_view fmt)
{
  constexpr std::array valid_formats = {"parquet", "csv"};
  if (!std::ranges::contains(valid_formats, fmt)) [[unlikely]] {
    throw std::invalid_argument(
      std::format("Invalid format: {} (allowed: {})", 
                 fmt, fmt::join(valid_formats, ", "))
    );
  }
}

#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/output_context.hpp>
#include <parquet/arrow/writer.h>
#include <spdlog/spdlog.h>

namespace
{

using namespace gtopt;

template<ArrowBuildable Type = arrow::DoubleType,
         typename Values,
         typename Valids = std::vector<bool>>
constexpr auto make_array(Values&& values, Valids&& valids = {})
{
  typename arrow::CTypeTraits<Type>::BuilderType builder;

  auto st = valids.empty() ? builder.AppendValues(std::forward<Values>(values))
                           : builder.AppendValues(std::forward<Values>(values),
                                                  std::forward<Valids>(valids));
  if (!st.ok()) {
    SPDLOG_CRITICAL("can't append values");
    throw std::runtime_error("can't append values");
  }

  ArrowArray array;
  if (!builder.Finish(&array).ok()) {
    SPDLOG_CRITICAL("can't build values");
    throw std::runtime_error("can't build values");
  }

  return array;
}

using str = std::string;

template<typename Type = Uid, typename STBUids>
  requires std::same_as<std::remove_cvref_t<STBUids>, STBUids>
[[nodiscard]] constexpr auto make_stb_prelude(STBUids&& stb_uids)
{
  std::vector<ArrowField> fields = {
      arrow::field(str {Scenario::class_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Stage::class_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Block::class_name}, ArrowTraits<Type>::type())};

  std::vector<ArrowArray> arrays = {
      make_array<Type>(std::forward<STBUids>(stb_uids).scenario_uids),
      make_array<Type>(std::forward<STBUids>(stb_uids).stage_uids),
      make_array<Type>(std::forward<STBUids>(stb_uids).block_uids)};

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = Uid, typename STUids>
  requires std::same_as<std::remove_cvref_t<STUids>, STUids>
[[nodiscard]] constexpr auto make_st_prelude(STUids&& st_uids)
{
  std::vector<ArrowField> fields = {
      arrow::field(str {Scenario::class_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Stage::class_name}, ArrowTraits<Type>::type())};

  std::vector<ArrowArray> arrays = {
      make_array<Type>(std::forward<STUids>(st_uids).scenario_uids),
      make_array<Type>(std::forward<STUids>(st_uids).stage_uids)};

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = Uid, typename TUids>
  requires std::same_as<std::remove_cvref_t<TUids>, TUids>
[[nodiscard]] constexpr auto make_t_prelude(TUids&& t_uids)
{
  std::vector<ArrowField> fields = {
      arrow::field(str {Stage::class_name}, ArrowTraits<Type>::type())};

  std::vector<ArrowArray> arrays = {
      make_array<Type>(std::forward<TUids>(t_uids).stage_uids)};

  return std::pair {std::move(fields), std::move(arrays)};
}

template<typename Type = double, typename FieldVector>
constexpr auto make_field_arrays(FieldVector&& field_vector)
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

    // Procesar prelude solo una vez
    if (!prelude_processed && prelude) [[likely]] {
      auto&& [pfields, parrays] = *std::forward<decltype(prelude)>(prelude);

      // Mover si es rvalue, copiar si es lvalue
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
constexpr auto make_table(FieldVector&& field_vector)
    -> arrow::Result<std::shared_ptr<arrow::Table>>
{
  auto [fields, arrays] =
      make_field_arrays<Type>(std::forward<FieldVector>(field_vector));

  return arrow::Table::Make(arrow::schema(std::move(fields)),
                            std::move(arrays));
}

constexpr auto parquet_write_table(const auto& fpath,
                                   const auto& table,
                                   const auto& zfmt)
{
  arrow::Status status;
  const auto filename = fpath.string() + ".parquet";
  PARQUET_ASSIGN_OR_THROW(auto output,
                          arrow::io::FileOutputStream::Open(filename));

  parquet::WriterProperties::Builder props_builder;

  using codec_t = decltype(parquet::Compression::UNCOMPRESSED);
  static const std::map<std::string, codec_t> codec_map = {
      {"uncompressed", parquet::Compression::UNCOMPRESSED},
      {"gzip", parquet::Compression::GZIP},
      {"zstd", parquet::Compression::ZSTD},
      {"lzo", parquet::Compression::LZO}};

  props_builder.compression(
      get_optvalue(codec_map, zfmt).value_or(parquet::Compression::GZIP));

  const auto props = props_builder.build();

  status = parquet::arrow::WriteTable(*table.get(),
                                      arrow::default_memory_pool(),
                                      output,
                                      1024 * 1024,  // NOLINT
                                      props);
  if (!status.ok()) [[unlikely]] {
    auto msg = std::format("File write failed: {}", fpath.string());
    SPDLOG_CRITICAL("{}", msg);
    throw std::runtime_error(std::move(msg));
  }
  return status;
}

constexpr auto csv_write_table(const auto& fpath,
                               const auto& table,
                               const auto& /* zfmt */)
{
  arrow::Status status;
  const auto filename = fpath.string() + ".csv";
  ARROW_ASSIGN_OR_RAISE(auto output,
                        arrow::io::FileOutputStream::Open(filename));

  const auto write_options = arrow::csv::WriteOptions::Defaults();
  status = WriteCSV(*table.get(), write_options, output.get());
  if (!status.ok()) {
    const auto msg = std::format("can' t write to file {}", fpath.string());
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  return status;
}

constexpr auto write_table(std::string_view fmt,
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
constexpr auto create_tables(auto&& output_directory, auto&& field_vector_map)
{
  using PathTable =
      std::pair<std::filesystem::path, std::shared_ptr<arrow::Table>>;

  std::vector<PathTable> path_tables;

  const auto dirpath = std::filesystem::path(output_directory);
  for (auto&& [class_fname, vfields] : field_vector_map) {
    auto&& [cname, fname] = class_fname;
    const auto mtable = make_table<Type>(vfields);

    const auto dpath = dirpath / cname;

    std::filesystem::create_directories(dpath);

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
  const auto zfmt = options().compression_format();
  const auto& path_tables =
      create_tables(options().output_directory(), field_vector_map);

  std::vector<std::jthread> tasks;
  tasks.reserve(path_tables.size());
  for (auto&& [path, table] : path_tables) {
    tasks.emplace_back([path, table, fmt, zfmt] {
      if (!write_table(fmt, path, table, zfmt).ok()) [[unlikely]] {
        auto msg = std::format("File write failed: {}", path.string());
        SPDLOG_CRITICAL("{}", msg);
        throw std::runtime_error(std::move(msg));
      }
    });
  }
  for (auto&& t : tasks) {
    t.join();
  }

  const auto sol_path =
      std::filesystem::path(options().output_directory()) / "solution.csv";

  std::ofstream sol_file(sol_path.string());
  sol_file << std::format("{:>12},{}\n{:>12},{}\n{:>12},{}",
                         "obj_value", sol_obj_value,
                         "kappa", sol_kappa,
                         "status", sol_status);
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
