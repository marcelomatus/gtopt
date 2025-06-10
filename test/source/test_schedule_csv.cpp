#include <cstdio>  // Para std::remove
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

struct TestData
{
  int stage;
  int block;
  double uid_1;
};

TEST_CASE("CSV file write and read test")
{
  const std::string filename = "test_data.csv";

  // Datos de prueba
  std::vector<TestData> test_data = {
      {0, 0, 1.0}, {1, 0, 2.0}, {0, 1, 3.0}, {1, 1, 4.0}};

  SUBCASE("0 Write CSV file")
  {
    std::ofstream file(filename);
    REQUIRE(file.is_open());

    // Escribir header
    file << "stage,block,uid_1\n";

    // Escribir datos
    for (const auto& row : test_data) {
      file << row.stage << "," << row.block << "," << std::fixed
           << std::setprecision(1) << row.uid_1 << "\n";
    }

    file.close();
    REQUIRE(file.good());

    // Verificar que el archivo existe y tiene contenido
    std::ifstream check_file(filename);
    REQUIRE(check_file.is_open());

    std::string content((std::istreambuf_iterator<char>(check_file)),
                        std::istreambuf_iterator<char>());
    check_file.close();

    CHECK(!content.empty());
    CHECK(content.find("stage,block,uid_1") != std::string::npos);
    CHECK(content.find("0,0,1.0") != std::string::npos);
    CHECK(content.find("1,0,2.0") != std::string::npos);
    CHECK(content.find("0,1,3.0") != std::string::npos);
    CHECK(content.find("1,1,4.0") != std::string::npos);
  }

  SUBCASE("1 Read and verify CSV file")
  {
    std::ifstream file(filename);
    REQUIRE(file.is_open());

    std::string line;
    std::vector<TestData> read_data;

    // Leer header
    std::getline(file, line);
    CHECK(line == "stage,block,uid_1");

    // Leer datos
    while (std::getline(file, line)) {
      std::stringstream ss(line);
      std::string cell;
      TestData row {};

      // Parsear stage
      std::getline(ss, cell, ',');
      row.stage = std::stoi(cell);

      // Parsear block
      std::getline(ss, cell, ',');
      row.block = std::stoi(cell);

      // Parsear uid_1
      std::getline(ss, cell, ',');
      row.uid_1 = std::stod(cell);

      read_data.push_back(row);
    }

    file.close();

    // Verificar cantidad de filas
    CHECK(read_data.size() == 4);

    // Verificar datos específicos
    CHECK(read_data[0].stage == 0);
    CHECK(read_data[0].block == 0);
    CHECK(read_data[0].uid_1 == doctest::Approx(1.0));

    CHECK(read_data[1].stage == 1);
    CHECK(read_data[1].block == 0);
    CHECK(read_data[1].uid_1 == doctest::Approx(2.0));

    CHECK(read_data[2].stage == 0);
    CHECK(read_data[2].block == 1);
    CHECK(read_data[2].uid_1 == doctest::Approx(3.0));

    CHECK(read_data[3].stage == 1);
    CHECK(read_data[3].block == 1);
    CHECK(read_data[3].uid_1 == doctest::Approx(4.0));

    // Verificar que todos los datos coinciden
    for (size_t i = 0; i < test_data.size(); ++i) {
      CHECK(read_data[i].stage == test_data[i].stage);
      CHECK(read_data[i].block == test_data[i].block);
      CHECK(read_data[i].uid_1 == doctest::Approx(test_data[i].uid_1));
    }

    // Limpiar archivo de prueba al final
    int remove_result = std::remove(filename.c_str());
    CHECK(remove_result == 0);
  }
}
