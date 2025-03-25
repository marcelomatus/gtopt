#include <exception>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/cast.h>
#include <arrow/csv/api.h>
#include <arrow/dataset/dataset.h>
#include <arrow/dataset/discovery.h>
#include <arrow/dataset/file_base.h>
#include <arrow/dataset/file_ipc.h>
#include <arrow/dataset/file_parquet.h>
#include <arrow/dataset/scanner.h>
#include <arrow/filesystem/filesystem.h>
#include <arrow/result.h>
#include <arrow/scalar.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/type_fwd.h>
#include <arrow/type_traits.h>
#include <arrow/util/iterator.h>
#include <easylogging++.h>
#include <gtopt/output_context.hpp>
#include <parquet/arrow/writer.h>

namespace gtopt
{

template<typename Type = arrow::DoubleType,
         typename Values,
         typename Valids = std::vector<bool>>
inline auto make_array(Values&& values, Valids&& valids = {})
{
  typename arrow::CTypeTraits<Type>::BuilderType builder;

  auto st = valids.empty() ? builder.AppendValues(std::forward<Values>(values))
                           : builder.AppendValues(std::forward<Values>(values),
                                                  std::forward<Valids>(valids));
  if (!st.ok()) {
    const auto msg = fmt::format("can't append values");
    LOG(FATAL) << msg;  // NOLINT
  }

  ArrowArray array;
  if (!builder.Finish(&array).ok()) {
    const auto msg = fmt::format("can't build values");
    LOG(FATAL) << msg;  // NOLINT
  }

  return array;
}

using str = std::string;

template<typename Type = Uid>
inline auto make_stb_prelude(auto&& stb_active_uids)
{
  const std::vector<ArrowField> fields = {
      arrow::field(str {Scenery::column_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Stage::column_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Block::column_name}, ArrowTraits<Type>::type())};

  const std::vector<ArrowArray> arrays = {
      make_array<Type>(std::get<0>(stb_active_uids)),
      make_array<Type>(std::get<1>(stb_active_uids)),
      make_array<Type>(std::get<2>(stb_active_uids))};

  return std::make_pair(fields, arrays);
}

template<typename Type = Uid>
inline auto make_st_prelude(auto&& st_active_uids)
{
  const std::vector<ArrowField> fields = {
      arrow::field(str {Scenery::column_name}, ArrowTraits<Type>::type()),
      arrow::field(str {Stage::column_name}, ArrowTraits<Type>::type())};

  const std::vector<ArrowArray> arrays = {
      make_array<Type>(std::get<0>(st_active_uids)),
      make_array<Type>(std::get<1>(st_active_uids))};

  return std::make_pair(fields, arrays);
}

template<typename Type = Uid>
inline auto make_t_prelude(auto&& t_active_uids)
{
  const std::vector<ArrowField> fields = {
      arrow::field(str {Stage::column_name}, ArrowTraits<Type>::type())};

  const std::vector<ArrowArray> arrays = {make_array<Uid>(t_active_uids)};

  return std::make_pair(fields, arrays);
}

template<typename Type = double>
constexpr auto make_field_arrays(auto&& field_vector)
{
  std::vector<ArrowField> fields;
  fields.reserve(field_vector.size() + 3);
  std::vector<ArrowArray> arrays;
  arrays.reserve(field_vector.size() + 3);

  for (bool first = true;
       auto&& [fname, fvalues, fvalids, prelude] : field_vector)
  {
    if (fvalues.empty()) {
      continue;
    }
    if (first && prelude) {
      auto&& [pfields, parrays] = *prelude;
      fields = pfields;
      arrays = parrays;
      first = false;
    }
    fields.emplace_back(arrow::field(fname, ArrowTraits<Type>::type()));
    arrays.emplace_back(make_array<Type>(fvalues, fvalids));
  }

  return std::make_pair(fields, arrays);
}

template<typename Type = double>
inline auto make_table(auto&& field_vector)
    -> arrow::Result<std::shared_ptr<arrow::Table>>
{
  const auto& [fields, arrays] = make_field_arrays<Type>(field_vector);
  return arrow::Table::Make(arrow::schema(fields), arrays);
}

inline auto parquet_write_table(const auto& fpath,
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
  if (!status.ok()) {
    const auto msg = fmt::format("can' t write to file {}", fpath.string());
    LOG(ERROR) << msg;  // NOLINT
    throw std::runtime_error(msg);
  }
  return status;
}

inline auto csv_write_table(const auto& fpath,
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
    const auto msg = fmt::format("can' t write to file {}", fpath.string());
    LOG(ERROR) << msg;  // NOLINT
    throw std::runtime_error(msg);
  }

  return status;
}

inline auto write_table(std::string_view fmt,
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
inline auto create_tables(auto&& output_directory, auto&& field_vector_map)
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

void OutputContext::write() const
{
  const auto fmt = options().output_format();
  const auto zfmt = options().compression_format();
  const auto& path_tables =
      create_tables(options().output_directory(), field_vector_map);

  std::vector<std::thread> tasks;
  tasks.reserve(path_tables.size());
  for (auto&& [path, table] : path_tables) {
    tasks.emplace_back(
        [&]
        {
          if (!write_table(fmt, path, table, zfmt).ok()) {
            LOG(ERROR) << "can't write file " << path.string();  // NOLINT
          }
        });
  }
  for (auto&& t : tasks) {
    t.join();
  }

  const auto sol_path =
      std::filesystem::path(options().output_directory()) / "solution.csv";

  std::ofstream sol_file(sol_path.string());
  sol_file << "parameter,value" << '\n';
  sol_file << "obj_value," << sol_obj_value << '\n';
  sol_file << "kappa," << sol_kappa << '\n';
  sol_file << "status," << sol_status << '\n';
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
    , block_cost_factors(psc.block_cost_factors())
    , stage_cost_factors(psc.stage_cost_factors())
    , stb_prelude(make_stb_prelude(psc.stb_active_uids()))
    , st_prelude(make_st_prelude(psc.st_active_uids()))
    , t_prelude(make_t_prelude(psc.t_active_uids()))
{
}

}  // namespace gtopt
