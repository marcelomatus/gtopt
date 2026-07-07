/**
 * @file      test_output_context.hpp
 * @brief     Unit tests for OutputContext (output writing and formatting)
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>
#include <zlib.h>
#include <zstd.h>

using namespace gtopt;

namespace  // NOLINT
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
  using namespace gtopt;
  auto [system, simulation] = make_basic_system();

  // Use a temp directory for output
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_test_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Call write_out which exercises OutputContext
  system_lp.write_out();

  // Verify that output tables were written (solution.csv is now written
  // by PlanningLP::write_out(), not SystemLP::write_out())
  CHECK(std::filesystem::exists(tmpdir / "Generator"));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - write output as CSV")
{
  auto [system, simulation] = make_basic_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_csv_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  // Verify output tables were written (solution.csv is written by
  // PlanningLP::write_out(), not SystemLP::write_out())
  CHECK(std::filesystem::exists(tmpdir / "Generator"));

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
          .reserve_zones = {SingleId {Uid {1}}},
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.model_options.reserve_shortage_cost = 10000.0;

  const System system = {
      .name = "ReserveOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .reserve_zone_array = reserve_zone_array,
      .reserve_provision_array = reserve_provision_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  // Verify output tables were written (solution.csv is written by
  // PlanningLP::write_out(), not SystemLP::write_out())
  CHECK(std::filesystem::exists(tmpdir / "Generator"));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - write output with hydro and seepage")
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
          .production_factor = 1.0,
      },
  };

  const Array<ReservoirSeepage> reservoir_seepage_array = {
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const System system = {
      .name = "HydroOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
      .reservoir_seepage_array = reservoir_seepage_array,
      .turbine_array = turbine_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  // Verify output tables were written (solution.csv is written by
  // PlanningLP::write_out(), not SystemLP::write_out())
  CHECK(std::filesystem::exists(tmpdir / "Generator"));

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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.model_options.demand_fail_cost = 10000.0;

  const System system = {
      .name = "ProfileOutputTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .generator_profile_array = generator_profile_array,
      .demand_profile_array = demand_profile_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  // Verify output tables were written (solution.csv is written by
  // PlanningLP::write_out(), not SystemLP::write_out())
  CHECK(std::filesystem::exists(tmpdir / "Generator"));

  // Clean up
  std::filesystem::remove_all(tmpdir);
}

// ---------------------------------------------------------------------------
// CSV compression behaviour tests
// ---------------------------------------------------------------------------

namespace  // NOLINT
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

// Decompress a zstd file into a string. Returns empty string on failure.
std::string zstd_decompress_to_string(const std::filesystem::path& zst_path)
{
  std::ifstream src(zst_path, std::ios::binary | std::ios::ate);
  if (!src.is_open()) {
    return {};
  }
  const auto file_size = static_cast<std::size_t>(src.tellg());
  src.seekg(0, std::ios::beg);
  std::vector<char> compressed(file_size);
  if (!src.read(compressed.data(), static_cast<std::streamsize>(file_size))) {
    return {};
  }
  src.close();

  // Use streaming decompression (handles Arrow's streaming zstd output).
  // RAII wrapper ensures ZSTD_DCtx is freed even on early return.
  const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx(
      ZSTD_createDCtx(), &ZSTD_freeDCtx);
  if (!dctx) {
    return {};
  }

  std::string result;
  constexpr std::size_t kOutBufSize = 65536;
  std::vector<char> out_buf(kOutBufSize);

  ZSTD_inBuffer input {.src = compressed.data(), .size = file_size, .pos = 0};
  while (input.pos < input.size) {
    ZSTD_outBuffer output {
        .dst = out_buf.data(),
        .size = kOutBufSize,
        .pos = 0,
    };
    const auto ret = ZSTD_decompressStream(dctx.get(), &output, &input);
    if (ZSTD_isError(ret) != 0U) {
      return {};
    }
    result.append(out_buf.data(), output.pos);
  }
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

TEST_CASE(
    "OutputContext - CSV with snappy codec falls back to plain "
    ".csv")  // NOLINT
{
  // `snappy` is not a streaming codec the `csv_write_table` path
  // supports, so when a user explicitly asks for CSV format + snappy
  // compression the writer falls back to uncompressed `.csv`.  Test
  // pins `snappy` explicitly because the post-2026-05-19 default
  // `output_compression` is `zstd` — exercising the default would
  // hit the `.csv.zst` branch covered by the next test.  This case
  // documents the snappy fallback the codec table relies on.
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_snappy_fallback";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::snappy;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  bool found_plain_csv = false;
  bool found_zst = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    const auto p = entry.path().string();
    if (p.ends_with(".csv")) {
      found_plain_csv = true;
    }
    if (p.ends_with(".csv.zst")) {
      found_zst = true;
    }
  }
  CHECK(found_plain_csv);
  CHECK_FALSE(found_zst);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("OutputContext - CSV explicit zstd compression")  // NOLINT
{
  // Users who want compressed CSV opt in explicitly.  Verifies the
  // CSV writer's `zstd` branch still produces `.csv.zst` files.
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_explicit_zst";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::zstd;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  bool found_zst = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().string().ends_with(".csv.zst")) {
      found_zst = true;
    }
  }
  CHECK(found_zst);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(
    "OutputContext - CSV no compression (explicit uncompressed)")  // NOLINT
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_csv_nocomp";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression =
      CompressionCodec::uncompressed;  // explicit no-compression

  const PlanningOptionsLP options(opts);
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::gzip;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // .csv.gz files must exist; no plain *.csv data files (solution.csv is
  // written by PlanningLP::write_out(), not SystemLP::write_out())
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::gzip;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // Long output drops zero rows, so a field with no non-zero dispatch is a
  // header-only CSV that the strict `csv_read_table` rejects.  Scan every
  // gzip CSV shard and confirm at least one round-trips with data rows.
  bool found_readable = false;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    const auto p = entry.path().string();
    constexpr std::string_view kSuffix {".csv.gz"};
    if (!p.ends_with(kSuffix)) {
      continue;
    }
    const std::filesystem::path stem = p.substr(0, p.size() - kSuffix.size());
    try {
      const auto table = csv_read_table(stem);
      if (table.has_value() && (*table)->num_rows() > 0) {
        found_readable = true;
        break;
      }
    } catch (const std::exception&) {
      // header-only all-zero field — keep scanning for one with data.
    }
  }
  CHECK(found_readable);

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE(  // NOLINT
    "OutputContext - CSV zstd compression produces .csv.zst")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_csv_zstd";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::zstd;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());
  system_lp.write_out();

  // .csv.zst files must exist (zstd is now natively supported for CSV)
  bool found_zst = false;
  std::filesystem::path first_zst;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(tmpdir))
  {
    if (entry.path().string().ends_with(".csv.zst")) {
      found_zst = true;
      first_zst = entry.path();
    }
  }
  CHECK(found_zst);

  // Decompress the first .csv.zst and verify it contains valid CSV content
  if (found_zst) {
    const auto content = zstd_decompress_to_string(first_zst);
    CHECK_FALSE(content.empty());
    // CSV must have at least a header row with comma-separated columns
    CHECK(content.find(',') != std::string::npos);
  }

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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_compression = CompressionCodec::gzip;

  const PlanningOptionsLP options(opts);
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_compression =
      CompressionCodec::snappy;  // not in codec_map → triggers fallback

  const PlanningOptionsLP options(opts);
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
    "OutputContext - Parquet unsupported codec (lzo) falls back to zstd")
{
  auto [system, simulation] = make_csv_system();
  const auto tmpdir = std::filesystem::temp_directory_path() / "gtopt_pq_lzo";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.output_compression =
      CompressionCodec::lzo;  // known in codec_map but unsupported

  const PlanningOptionsLP options(opts);
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
namespace  // NOLINT
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;

  const PlanningOptionsLP options(opts);
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;

  const PlanningOptionsLP options(opts);
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const PlanningOptionsLP options(opts);
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

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::csv;
  opts.output_compression = CompressionCodec::gzip;

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  REQUIRE(lp.resolve().has_value());

  CHECK_NOTHROW(system_lp.write_out());

  restore_and_remove(tmpdir);
}

// ---------------------------------------------------------------------------
// probe_parquet_codec direct tests
// ---------------------------------------------------------------------------

TEST_CASE("probe_parquet_codec - empty string returns empty")  // NOLINT
{
  CHECK(probe_parquet_codec("").empty());
}

TEST_CASE("probe_parquet_codec - none returns none")  // NOLINT
{
  CHECK(probe_parquet_codec("none") == "none");
}

TEST_CASE("probe_parquet_codec - uncompressed returns uncompressed")  // NOLINT
{
  CHECK(probe_parquet_codec("uncompressed") == "uncompressed");
}

TEST_CASE("probe_parquet_codec - gzip returns gzip")  // NOLINT
{
  // gzip is always available in Arrow
  CHECK(probe_parquet_codec("gzip") == "gzip");
}

TEST_CASE("probe_parquet_codec - zstd returns zstd")  // NOLINT
{
  CHECK(probe_parquet_codec("zstd") == "zstd");
}

TEST_CASE("probe_parquet_codec - unknown codec falls back")  // NOLINT
{
  // "xyzzy" is not in the codec_map → should warn and fall back
  const auto result = probe_parquet_codec("xyzzy");
  // Falls back to gzip if available, else ""
  CHECK((result == "gzip" || result.empty()));
}

TEST_CASE("probe_parquet_codec - lzo typically unavailable")  // NOLINT
{
  // lzo is in codec_map but usually not compiled into Arrow
  const auto result = probe_parquet_codec("lzo");
  // Falls back to gzip or "" if lzo is unavailable
  CHECK((result == "lzo" || result == "gzip" || result.empty()));
}

// ---------------------------------------------------------------------------
// OutputContext — ScaledView auto-descaling through output pipeline
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "OutputContext - ScaledView descaled values appear in output files")
{
  using namespace gtopt;

  // Build a system with a single scaled generator
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> gen_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
  };
  const Array<Demand> dem_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_scaled_out";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;

  const System system = {
      .name = "ScaledOutputTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  // Verify that ScaledView returns physical values, not raw LP values.
  // get_col_sol() returns ScaledView (physical = LP × col_scale).
  // For unscaled columns (scale=1), physical == LP.
  const auto col_sol = lp.get_col_sol();
  const auto col_sol_raw = lp.get_col_sol_raw();
  CHECK(!col_sol.empty());
  CHECK(col_sol.size() == col_sol_raw.size());

  // For each column: physical == raw * scale.
  // With no VariableScale entries, all scales default to 1.0.
  for (const auto ci : iota_range<ColIndex>(0, col_sol.size())) {
    const double scale = lp.get_col_scale(ci);
    CHECK(col_sol[ci] == doctest::Approx(col_sol_raw[ci] * scale));
  }

  // Write output and verify files exist
  system_lp.write_out();
  CHECK(std::filesystem::exists(tmpdir / "Generator"));

  std::filesystem::remove_all(tmpdir);
}

// ---------------------------------------------------------------------------
// OutputContext — multiple generators produce grouped output
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "OutputContext - multiple generators produce correctly grouped fields")
{
  using namespace gtopt;

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Generator> gen_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 200.0,
      },
      {
          .uid = Uid {2},
          .name = "g2",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 100.0,
      },
  };
  const Array<Demand> dem_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 80.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_multi_gen";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.write_out = OutputFlags::all;  // explicitly emit reduced_cost columns

  const System system = {
      .name = "MultiGenTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
      .generator_array = gen_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  // Both generators should produce sol and cost outputs grouped under
  // Generator/
  const auto gen_dir = tmpdir / "Generator";
  CHECK(std::filesystem::exists(gen_dir));

  // generation_sol and generation_cost files should exist with columns for
  // both generators (as_label joins with '_')
  CHECK(std::filesystem::exists(gen_dir / "generation_sol.parquet"));
  CHECK(std::filesystem::exists(gen_dir / "generation_cost.parquet"));

  std::filesystem::remove_all(tmpdir);
}

// ---------------------------------------------------------------------------
// OutputContext — empty holder produces no output file
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "OutputContext - empty system produces no output directories")
{
  using namespace gtopt;

  // System with bus and demand only — no generators, no reservoirs
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> dem_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 50.0,
      },
  };

  const Simulation simulation = {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_empty";
  std::filesystem::create_directories(tmpdir);

  PlanningOptions opts;
  opts.output_directory = tmpdir.string();
  opts.output_format = DataFormat::parquet;
  opts.model_options.demand_fail_cost =
      1000.0;  // make LP feasible without generators

  const System system = {
      .name = "EmptyTest",
      .bus_array = bus_array,
      .demand_array = dem_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP simulation_lp(simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());

  system_lp.write_out();

  // No Generator or Reservoir directory should be created
  CHECK(!std::filesystem::exists(tmpdir / "Generator"));
  CHECK(!std::filesystem::exists(tmpdir / "Reservoir"));

  // But Demand and Bus should exist (they have output columns)
  CHECK(std::filesystem::exists(tmpdir / "Demand"));

  std::filesystem::remove_all(tmpdir);
}

// ---------------------------------------------------------------------------
// Coverage for the throwaway-rebuild path:
// `rebuild_collections_if_needed` runs ONLY when `low_memory_mode != off`.
// Existing tests above all default to `off`, so the per-cell rebuild that
// `SystemLP::write_out` performs under `compress` mode (and the overrides
// added 2026-05-16 — equilibration=none, compute_stats=false, names off,
// scale_objective=1.0, validation off — see `system_lp.cpp:952`) was
// otherwise untested.  The test below forces the release+rebuild round
// trip and asserts the resulting parquet matches the `off`-mode baseline.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "OutputContext - write_out under low_memory=compress matches off baseline")
{
  using namespace gtopt;
  auto [system, simulation] = make_basic_system();

  const auto tmpdir =
      std::filesystem::temp_directory_path() / "gtopt_test_compress_writeout";
  const auto off_dir = tmpdir / "off";
  const auto compress_dir = tmpdir / "compress";
  std::filesystem::create_directories(off_dir);
  std::filesystem::create_directories(compress_dir);

  auto run = [&](const std::filesystem::path& out_dir, LowMemoryMode mode)
  {
    PlanningOptions opts;
    opts.output_directory = out_dir.string();
    opts.output_format = DataFormat::parquet;
    opts.lp_matrix_options.low_memory_mode = mode;
    if (mode != LowMemoryMode::off) {
      opts.lp_matrix_options.memory_codec = CompressionCodec::lz4;
    }

    const PlanningOptionsLP options(opts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);

    if (mode != LowMemoryMode::off) {
      // Drop the live backend so the subsequent `write_out` is forced
      // through `rebuild_collections_if_needed` (which re-flattens
      // with the 2026-05-16 throwaway overrides).  Without this
      // release, the disposable collections remain populated and the
      // rebuild path is bypassed — exactly the gap left by every
      // pre-existing `test_output_context` case (all default off).
      sys_lp.release_backend();
    }

    sys_lp.write_out();
  };

  run(off_dir, LowMemoryMode::off);
  run(compress_dir, LowMemoryMode::compress);

  // Both modes must have produced the same set of element class
  // directories — compress's rebuild-then-emit must not drop any
  // emitter that off's direct-emit produced.
  CHECK(std::filesystem::exists(off_dir / "Generator"));
  CHECK(std::filesystem::exists(compress_dir / "Generator"));
  CHECK(std::filesystem::exists(off_dir / "Demand"));
  CHECK(std::filesystem::exists(compress_dir / "Demand"));
  CHECK(std::filesystem::exists(off_dir / "Bus"));
  CHECK(std::filesystem::exists(compress_dir / "Bus"));

  // The two output trees must have the same shape: same set of
  // (relative) file paths.  Byte-comparing parquet payloads is
  // fragile across Arrow versions (compression, metadata), so we
  // check the path set — the throwaway-rebuild path must emit
  // every shard the live-backend path emits, no more no less.
  auto collect = [](const std::filesystem::path& root)
  {
    std::vector<std::filesystem::path> rels;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
      if (e.is_regular_file()) {
        rels.push_back(std::filesystem::relative(e.path(), root));
      }
    }
    std::ranges::sort(rels);
    return rels;
  };
  const auto off_files = collect(off_dir);
  const auto compress_files = collect(compress_dir);
  CHECK(off_files == compress_files);

  std::filesystem::remove_all(tmpdir);
}
