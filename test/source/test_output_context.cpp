/**
 * @file      test_output_context.cpp
 * @brief     Unit tests for OutputContext (output writing and formatting)
 * @date      2026-02-19
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

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
    std::string content((std::istreambuf_iterator<char>(f)),
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
