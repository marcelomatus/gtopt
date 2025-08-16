#include <filesystem>
#include <memory>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/exception.h>

using namespace gtopt;

TEST_CASE("Parquet file write and read test")
{
  const std::string iname = "input";
  const std::string cname = "test_data";
  const std::string dirname = iname + "/" + cname + "/";
  const std::string fname = "field";
  const std::string filename = dirname + fname + ".parquet";

  CHECK(filename == "input/test_data/field.parquet");

  // Create directory if it doesn't exist
  std::filesystem::create_directories(dirname);

  SUBCASE("Write Parquet file")
  {
    // Crear datos de prueba
    const std::vector<Uid> scenario_data = {1, 1, 1, 2, 2, 2};
    const std::vector<Uid> stage_data = {1, 2, 2, 1, 2, 2};
    const std::vector<Uid> block_data = {1, 2, 3, 1, 2, 3};
    const std::vector<double> uid_1_data = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};

    // Crear arrays de Arrow usando builders
    arrow::Int32Builder scenario_builder;
    arrow::Int32Builder stage_builder;
    arrow::Int32Builder block_builder;
    arrow::DoubleBuilder uid_1_builder;

    // Agregar datos a los builders
    auto scenario_status = scenario_builder.AppendValues(scenario_data);
    auto stage_status = stage_builder.AppendValues(stage_data);
    auto block_status = block_builder.AppendValues(block_data);
    auto uid_1_status = uid_1_builder.AppendValues(uid_1_data);

    REQUIRE(scenario_status.ok());
    REQUIRE(stage_status.ok());
    REQUIRE(block_status.ok());
    REQUIRE(uid_1_status.ok());

    // Finalizar arrays
    std::shared_ptr<arrow::Array> scenario_array;
    std::shared_ptr<arrow::Array> stage_array;
    std::shared_ptr<arrow::Array> block_array;
    std::shared_ptr<arrow::Array> uid_1_array;

    auto scenario_finish = scenario_builder.Finish(&scenario_array);
    auto stage_finish = stage_builder.Finish(&stage_array);
    auto block_finish = block_builder.Finish(&block_array);
    auto uid_1_finish = uid_1_builder.Finish(&uid_1_array);

    REQUIRE(scenario_finish.ok());
    REQUIRE(stage_finish.ok());
    REQUIRE(block_finish.ok());
    REQUIRE(uid_1_finish.ok());

    // Crear schema
    auto schema = arrow::schema({arrow::field("scenario", arrow::int32()),
                                 arrow::field("stage", arrow::int32()),
                                 arrow::field("block", arrow::int32()),
                                 arrow::field("uid_1", arrow::float64())});

    // Crear tabla
    auto table_result = arrow::Table::Make(
        schema, {scenario_array, stage_array, block_array, uid_1_array});

    REQUIRE(table_result != nullptr);
    CHECK(table_result->num_rows() == 6);
    CHECK(table_result->num_columns() == 4);

    // Escribir archivo Parquet
    auto output_stream_result = arrow::io::FileOutputStream::Open(filename);
    REQUIRE(output_stream_result.ok());
    const auto& output_stream = output_stream_result.ValueOrDie();

    auto write_status = parquet::arrow::WriteTable(*table_result,
                                                   arrow::default_memory_pool(),
                                                   output_stream,
                                                   /*chunk_size=*/1024);

    REQUIRE(write_status.ok());

    auto close_status = output_stream->Close();
    REQUIRE(close_status.ok());
  }

  SUBCASE("Read and verify Parquet file")
  {
    // Abrir archivo para lectura
    auto input_stream_result = arrow::io::ReadableFile::Open(filename);
    REQUIRE(input_stream_result.ok());
    const auto& input_stream = input_stream_result.ValueOrDie();

    // Crear reader de Parquet

    auto parquet_reader = parquet::ParquetFileReader::Open(input_stream);
    std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
    auto reader_status = parquet::arrow::FileReader::Make(
        arrow::default_memory_pool(), std::move(parquet_reader), &arrow_reader);
    REQUIRE(reader_status.ok());
    REQUIRE(arrow_reader != nullptr);

    // Leer tabla completa
    std::shared_ptr<arrow::Table> table;
    auto read_status = arrow_reader->ReadTable(&table);
    REQUIRE(read_status.ok());
    REQUIRE(table != nullptr);

    // Verificar estructura de la tabla
    CHECK(table->num_rows() == 6);
    CHECK(table->num_columns() == 4);

    // Verificar nombres de columnas
    auto schema = table->schema();
    CHECK(schema->field(0)->name() == "scenario");
    CHECK(schema->field(1)->name() == "stage");
    CHECK(schema->field(2)->name() == "block");
    CHECK(schema->field(3)->name() == "uid_1");

    // Verificar tipos de datos
    CHECK(schema->field(0)->type()->id() == arrow::Type::INT32);
    CHECK(schema->field(1)->type()->id() == arrow::Type::INT32);
    CHECK(schema->field(2)->type()->id() == arrow::Type::INT32);
    CHECK(schema->field(3)->type()->id() == arrow::Type::DOUBLE);

    // Obtener columnas
    auto scenario_column = table->column(0);
    auto stage_column = table->column(1);
    auto block_column = table->column(2);
    auto uid_1_column = table->column(3);

    // Verificar datos de la columna 'scenario'
    auto scenario_array =
        std::static_pointer_cast<arrow::Int32Array>(scenario_column->chunk(0));
    CHECK(scenario_array->Value(0) == 1);
    CHECK(scenario_array->Value(1) == 1);
    CHECK(scenario_array->Value(2) == 1);
    CHECK(scenario_array->Value(3) == 2);
    CHECK(scenario_array->Value(4) == 2);
    CHECK(scenario_array->Value(5) == 2);

    // Verificar datos de la columna 'stage'
    auto stage_array =
        std::static_pointer_cast<arrow::Int32Array>(stage_column->chunk(0));
    CHECK(stage_array->Value(0) == 1);
    CHECK(stage_array->Value(1) == 2);
    CHECK(stage_array->Value(2) == 2);
    CHECK(stage_array->Value(3) == 1);
    CHECK(stage_array->Value(4) == 2);
    CHECK(stage_array->Value(5) == 2);

    // Verificar datos de la columna 'block'
    auto block_array =
        std::static_pointer_cast<arrow::Int32Array>(block_column->chunk(0));
    CHECK(block_array->Value(0) == 1);
    CHECK(block_array->Value(1) == 2);
    CHECK(block_array->Value(2) == 3);
    CHECK(block_array->Value(3) == 1);
    CHECK(block_array->Value(4) == 2);
    CHECK(block_array->Value(5) == 3);

    // Verificar datos de la columna 'uid_1'
    auto uid_1_array =
        std::static_pointer_cast<arrow::DoubleArray>(uid_1_column->chunk(0));
    CHECK(uid_1_array->Value(0) == doctest::Approx(1.0));
    CHECK(uid_1_array->Value(1) == doctest::Approx(2.0));
    CHECK(uid_1_array->Value(2) == doctest::Approx(3.0));
    CHECK(uid_1_array->Value(3) == doctest::Approx(4.0));
    CHECK(uid_1_array->Value(4) == doctest::Approx(5.0));
    CHECK(uid_1_array->Value(5) == doctest::Approx(6.0));

    // Cerrar stream
    auto close_status = input_stream->Close();
    REQUIRE(close_status.ok());
  }

  SUBCASE("schedule parquet test")
  {
    using namespace gtopt;

    const Simulation sim = {
        .block_array = {{.uid = Uid {1}, .duration = 1},
                        {.uid = Uid {2}, .duration = 2},
                        {.uid = Uid {3}, .duration = 3}},
        .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1},
                        {.uid = Uid {2}, .first_block = 1, .count_block = 2}},
        .scenario_array = {{.uid = Uid {1}}, {.uid = Uid {2}}}};

    Options opt;
    opt.input_directory = iname;
    opt.input_format = "parquet";
    const OptionsLP options {opt};

    SimulationLP simulation {sim, options};

    const System sys;
    SystemLP system {sys, simulation};
    const SystemContext sc {simulation, system};
    const InputContext ic {sc};

    SUBCASE("stbfield")
    {
      const std::vector<std::vector<std::vector<double>>> vec = {{{1}, {2, 3}},
                                                                 {{4}, {5, 6}}};
      const STBRealFieldSched stbfield {cname + "@" + fname};

      const Id id {1, "uid_1"};

      const STBRealSched stbsched {ic, cname, id, stbfield};

      REQUIRE(stbsched.at(ScenarioUid {1}, StageUid {1}, BlockUid {1}) == 1);
      REQUIRE(stbsched.at(ScenarioUid {1}, StageUid {2}, BlockUid {2}) == 2);
      REQUIRE(stbsched.at(ScenarioUid {1}, StageUid {2}, BlockUid {3}) == 3);
      REQUIRE(stbsched.at(ScenarioUid {2}, StageUid {1}, BlockUid {1}) == 4);
      REQUIRE(stbsched.at(ScenarioUid {2}, StageUid {2}, BlockUid {2}) == 5);
      REQUIRE(stbsched.at(ScenarioUid {2}, StageUid {2}, BlockUid {3}) == 6);
    }

    // Clean up test files and directory
    SUBCASE("Cleanup test files")
    {
      REQUIRE(std::filesystem::remove(filename));
      REQUIRE(std::filesystem::remove(dirname));
    }
  }
}
