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
#include <gtopt/array_index_traits.hpp>
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
          .emin = 0.0,
          .emax = 10000.0,
          .eini = 5000.0,
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
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
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
  // output_compression not set → default "" → no compression

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
  opts.output_compression = "gzip";

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

TEST_CASE("OutputContext - CSV gzip output is readable through csv_read_table")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_gzip_readback";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  opts.output_compression = "gzip";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  const auto table = csv_read_table(tmpdir / "Generator" / "generation_sol");
  REQUIRE(table.has_value());
  CHECK((table && (*table)->num_rows() > 0));

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
  opts.output_compression = "zstd";  // not supported for CSV → falls back

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

// ---------------------------------------------------------------------------
// Parquet compression fallback tests
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "OutputContext - Parquet known supported codec (gzip) writes valid file")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_pq_gzip";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";
  opts.output_compression = "gzip";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // At least one .parquet file must exist
  bool found_parquet = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().extension() == ".parquet") {
      found_parquet = true;
    }
  }
  CHECK(found_parquet);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - Parquet unknown codec string falls back and writes file")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_pq_unknown";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";
  opts.output_compression = "snappy";  // not in codec_map → triggers fallback

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // Output must still exist despite the unsupported compression request
  bool found_parquet = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().extension() == ".parquet") {
      found_parquet = true;
    }
  }
  CHECK(found_parquet);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - Parquet unsupported codec (lzo) falls back to gzip")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_pq_lzo";
  std::filesystem::create_directories(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";
  opts.output_compression = "lzo";  // known in codec_map but unsupported

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  // Write must succeed (falls back to gzip or uncompressed) without throwing
  CHECK_NOTHROW(system_lp.write_out());

  bool found_parquet = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().extension() == ".parquet") {
      found_parquet = true;
    }
  }
  CHECK(found_parquet);

  std::filesystem::remove_all(tmpdir);
}

// ---------------------------------------------------------------------------
// Tests for output failure paths and solution.csv failure path
// ---------------------------------------------------------------------------
namespace
{
// Helper: create all expected output subdirectories inside @p outdir with
// read+exec but no write permission.  The component subdirs written by
// make_csv_system are: Bus, Generator, Demand.
void make_readonly_subdirs(const std::filesystem::path& outdir)
{
  for (const auto* sub : {"Bus", "Generator", "Demand"}) {
    auto p = outdir / sub;
    std::filesystem::create_directories(p);
    std::filesystem::permissions(
        p,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
  }
}

void restore_and_remove(const std::filesystem::path& outdir)
{
  // restore permissions recursively so remove_all works
  for (const auto& e : std::filesystem::recursive_directory_iterator(outdir)) {
    std::filesystem::permissions(e.path(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
  }
  std::filesystem::permissions(outdir,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add);
  std::filesystem::remove_all(outdir);
}
}  // namespace

TEST_CASE(  // NOLINT
    "OutputContext - write() with unwritable output dir logs error (no throw)")
{
  // Top-level dir is read-only → create_directories for subdirs fails
  // → create_tables skips them (covers lines 133, 347-350)
  // → write() completes without throwing.
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_readonly_out";
  std::filesystem::create_directories(tmpdir);
  std::filesystem::permissions(
      tmpdir,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  // write() should not throw even when the directory is not writable
  CHECK_NOTHROW(system_lp.write_out());

  // Restore permissions so cleanup succeeds
  std::filesystem::permissions(tmpdir,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - CSV write to read-only subdir logs error (no throw)")
{
  // Subdirs exist but are not writable.  create_directories succeeds (dir
  // already present), but FileOutputStream::Open fails and returns an error
  // status → write_table returns error → thread lambda logs SPDLOG_CRITICAL
  // (covers lines 256 and 385).
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_readonly_sub";
  std::filesystem::create_directories(tmpdir);
  make_readonly_subdirs(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  CHECK_NOTHROW(system_lp.write_out());

  restore_and_remove(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - Parquet write to read-only subdir logs error (no throw)")
{
  // Same pattern as CSV test, but with parquet format.
  // covers parquet_write_table Open failure (lines 228-231) and 385.
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_pq_readonly_sub";
  std::filesystem::create_directories(tmpdir);
  make_readonly_subdirs(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "parquet";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  CHECK_NOTHROW(system_lp.write_out());

  restore_and_remove(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - CSV gzip to unwritable dir logs error (no throw)")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_readonly_gz";
  std::filesystem::create_directories(tmpdir);
  make_readonly_subdirs(tmpdir);

  Options opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = "csv";
  opts.output_compression = "gzip";

  const OptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  CHECK_NOTHROW(system_lp.write_out());

  restore_and_remove(tmpdir);
}
