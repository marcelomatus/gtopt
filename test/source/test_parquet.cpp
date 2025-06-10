#include <iostream>
#include <memory>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/exception.h>

TEST_CASE("Parquet file write and read test")
{
  const std::string filename = "test_data.parquet";

  SUBCASE("Write Parquet file")
  {
    // Crear datos de prueba
    std::vector<int32_t> stage_data = {0, 1, 0, 1};
    std::vector<int32_t> block_data = {0, 0, 1, 1};
    std::vector<double> uid_1_data = {1.0, 2.0, 3.0, 4.0};

    // Crear arrays de Arrow usando builders
    arrow::Int32Builder stage_builder;
    arrow::Int32Builder block_builder;
    arrow::DoubleBuilder uid_1_builder;

    // Agregar datos a los builders
    auto stage_status = stage_builder.AppendValues(stage_data);
    auto block_status = block_builder.AppendValues(block_data);
    auto uid_1_status = uid_1_builder.AppendValues(uid_1_data);

    REQUIRE(stage_status.ok());
    REQUIRE(block_status.ok());
    REQUIRE(uid_1_status.ok());

    // Finalizar arrays
    std::shared_ptr<arrow::Array> stage_array;
    std::shared_ptr<arrow::Array> block_array;
    std::shared_ptr<arrow::Array> uid_1_array;

    auto stage_finish = stage_builder.Finish(&stage_array);
    auto block_finish = block_builder.Finish(&block_array);
    auto uid_1_finish = uid_1_builder.Finish(&uid_1_array);

    REQUIRE(stage_finish.ok());
    REQUIRE(block_finish.ok());
    REQUIRE(uid_1_finish.ok());

    // Crear schema
    auto schema = arrow::schema({arrow::field("stage", arrow::int32()),
                                 arrow::field("block", arrow::int32()),
                                 arrow::field("uid_1", arrow::float64())});

    // Crear tabla
    auto table_result =
        arrow::Table::Make(schema, {stage_array, block_array, uid_1_array});

    REQUIRE(table_result != nullptr);
    CHECK(table_result->num_rows() == 4);
    CHECK(table_result->num_columns() == 3);

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
    CHECK(table->num_rows() == 4);
    CHECK(table->num_columns() == 3);

    // Verificar nombres de columnas
    auto schema = table->schema();
    CHECK(schema->field(0)->name() == "stage");
    CHECK(schema->field(1)->name() == "block");
    CHECK(schema->field(2)->name() == "uid_1");

    // Verificar tipos de datos
    CHECK(schema->field(0)->type()->id() == arrow::Type::INT32);
    CHECK(schema->field(1)->type()->id() == arrow::Type::INT32);
    CHECK(schema->field(2)->type()->id() == arrow::Type::DOUBLE);

    // Obtener columnas
    auto stage_column = table->column(0);
    auto block_column = table->column(1);
    auto uid_1_column = table->column(2);

    // Verificar datos de la columna 'stage'
    auto stage_array =
        std::static_pointer_cast<arrow::Int32Array>(stage_column->chunk(0));
    CHECK(stage_array->Value(0) == 0);
    CHECK(stage_array->Value(1) == 1);
    CHECK(stage_array->Value(2) == 0);
    CHECK(stage_array->Value(3) == 1);

    // Verificar datos de la columna 'block'
    auto block_array =
        std::static_pointer_cast<arrow::Int32Array>(block_column->chunk(0));
    CHECK(block_array->Value(0) == 0);
    CHECK(block_array->Value(1) == 0);
    CHECK(block_array->Value(2) == 1);
    CHECK(block_array->Value(3) == 1);

    // Verificar datos de la columna 'uid_1'
    auto uid_1_array =
        std::static_pointer_cast<arrow::DoubleArray>(uid_1_column->chunk(0));
    CHECK(uid_1_array->Value(0) == doctest::Approx(1.0));
    CHECK(uid_1_array->Value(1) == doctest::Approx(2.0));
    CHECK(uid_1_array->Value(2) == doctest::Approx(3.0));
    CHECK(uid_1_array->Value(3) == doctest::Approx(4.0));

    // Cerrar stream
    auto close_status = input_stream->Close();
    REQUIRE(close_status.ok());
  }
}
