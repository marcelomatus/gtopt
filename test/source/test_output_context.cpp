/**
 * @file      test_output_context.cpp
 * @brief     Unit tests for OutputContext (output writing and formatting)
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <zlib.h>

using namespace gtopt;

namespace
{
auto make_basic_system()
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const System system = {
      .name = "OutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };

  return std::pair {system, simulation};
}
}  // namespace

TEST_CASE("OutputContext - write output after solve (parquet)")
{
  auto [system, simulation] = make_basic_system();

  // Use a temp directory for output
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_test_out";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Call write_out which exercises OutputContext
  system_lp.write_out();

  // Check solution.csv was created
  const auto sol_file = tmpdir / "solution.csv";
  CHECK(std::filesystem::exists(sol_file));

  // Check solution.csv content
  if (std::filesystem::exists(sol_file)) {
    std::ifstream f(sol_file);
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    CHECK(content.find("obj_value") != std::string::npos);
    CHECK(content.find("kappa") != std::string::npos);
    CHECK(content.find("status") != std::string::npos);
  }

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - write output as CSV")
{
  auto [system, simulation] = make_basic_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_csv_out";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  const auto sol_file = tmpdir / "solution.csv";
  CHECK(std::filesystem::exists(sol_file));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - write output with reserve components")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0},
  };

  const Array<ReserveZone> reserve_zone_array = {
      {
          .uid = Uid {1},
          .name = "rz1",
          .urreq = 50.0,
          .urcost = 1000.0,
      },
  };

  const Array<ReserveProvision> reserve_provision_array = {
      {
          .uid = Uid {1},
          .name = "rp1",
          .generator = Uid {1},
          .reserve_zones = "1",
          .urmax = 100.0,
          .ur_provision_factor = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_reserve_out";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";
  opts.reserve_fail_cost = 10000.0;

  const System system = {
      .name = "ReserveOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  const auto sol_file = tmpdir / "solution.csv";
  CHECK(std::filesystem::exists(sol_file));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - write output with hydro and filtration")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "hydro_gen",
          .bus = Uid {1},
          .gcost = 5.0,
          .capacity = 500.0,
      },
      {
          .uid = Uid {2},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 200.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 50.0},
  };

  const Array<Junction> junction_array = {
      {.uid = Uid {1}, .name = "j1"},
      {.uid = Uid {2}, .name = "j2", .drain = true},
  };

  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 500.0,
      },
  };

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 10000.0,
          .vmin = 0.0,
          .vmax = 10000.0,
          .vini = 5000.0,
      },
  };

  const Array<Turbine> turbine_array = {
      {
          .uid = Uid {1},
          .name = "tur1",
          .waterway = Uid {1},
          .generator = Uid {1},
          .conversion_rate = 1.0,
      },
  };

  const Array<Filtration> filtration_array = {
      {
          .uid = Uid {1},
          .name = "filt1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .slope = 0.001,
          .constant = 1.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_hydro_out";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";

  const System system = {
      .name = "HydroOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .filtration_array = filtration_array,
      .turbine_array = turbine_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  const auto sol_file = tmpdir / "solution.csv";
  CHECK(std::filesystem::exists(sol_file));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - write output with demand and generator profiles")
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 300.0,
      },
  };

  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 200.0},
  };

  const Array<DemandProfile> demand_profile_array = {
      {
          .uid = Uid {1},
          .name = "dp1",
          .demand = Uid {1},
          .profile = 0.8,
      },
  };

  const Array<GeneratorProfile> generator_profile_array = {
      {
          .uid = Uid {1},
          .name = "gp1",
          .generator = Uid {1},
          .profile = 0.7,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_profile_out";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  opts.demand_fail_cost = 10000.0;

  const System system = {
      .name = "ProfileOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .generator_profile_array = generator_profile_array,
      .demand_profile_array = demand_profile_array,
  };

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  const auto sol_file = tmpdir / "solution.csv";
  CHECK(std::filesystem::exists(sol_file));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

// ---------------------------------------------------------------------------
// CSV compression behaviour tests
// ---------------------------------------------------------------------------

namespace
{
// Decompress a gzip file into a string. Returns empty string on failure.
std::string gunzip_to_string(const std::filesystem::path& gz_path)
{
  gzFile gz = gzopen(gz_path.string().c_str(), "rb");  // NOLINT
  if (gz == nullptr) {
    return {};
  }
  std::string result;
  std::vector<char> buf(4096);  // NOLINT
  int n = 0;
  while ((n = gzread(gz, buf.data(), static_cast<unsigned>(buf.size()))) > 0) {
    result.append(buf.data(), static_cast<std::size_t>(n));
  }
  gzclose(gz);  // NOLINT
  return result;
}

auto make_csv_system()
{
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> generator_array = {{
      .uid = Uid {1},
      .name = "g1",
      .bus = Uid {1},
      .gcost = 50.0,
      .capacity = 300.0,
  }};
  const Array<Demand> demand_array = {
      {.uid = Uid {1}, .name = "d1", .bus = Uid {1}, .capacity = 100.0}};
  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };
  const System system = {
      .name = "CsvCompressionTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
  };
  return std::pair {system, simulation};
}
}  // namespace

TEST_CASE("OutputContext - CSV no compression (default)")  // NOLINT
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_nocomp";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  // compression_format not set → default "" → no compression

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // Plain *.csv files must exist; no *.csv.gz should exist
  bool found_csv = false;
  bool found_gz = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().extension() == ".csv") {
      found_csv = true;
    }
    if (entry.path().string().ends_with(".csv.gz")) {
      found_gz = true;
    }
  }
  CHECK(found_csv);
  CHECK_FALSE(found_gz);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - CSV gzip compression produces .csv.gz")  // NOLINT
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_csv_gzip";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  opts.compression_format = "gzip";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // .csv.gz files must exist; no plain *.csv data files (solution.csv is
  // written separately and is always uncompressed)
  bool found_gz = false;
  std::filesystem::path first_gz;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().string().ends_with(".csv.gz")) {
      found_gz = true;
      first_gz = entry.path();
    }
  }
  CHECK(found_gz);

  // Decompress the first .csv.gz and verify it contains valid CSV content
  if (found_gz) {
    const auto content = gunzip_to_string(first_gz);
    CHECK_FALSE(content.empty());
    // CSV must have at least a header row with comma-separated columns
    CHECK(content.find(',') != std::string::npos);
  }

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - CSV unsupported compression falls back to gzip")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_fallback";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  opts.compression_format = "zstd";  // not supported for CSV → falls back

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // Despite requesting "zstd", output must be .csv.gz (gzip fallback)
  bool found_gz = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().string().ends_with(".csv.gz")) {
      found_gz = true;
    }
  }
  CHECK(found_gz);

  std::filesystem::remove_all(tmpdir);
}
