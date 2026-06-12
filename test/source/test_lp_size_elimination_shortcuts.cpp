// SPDX-License-Identifier: BSD-3-Clause
//
// LP-size: column / row elimination shortcuts (critical-review follow-up).
//
// Each shortcut removes a provably-degenerate LP column or row and is
// paired with an explicit *write-out rule* — the value an output
// consumer should see for the elided entry.  For every shortcut here
// the eliminated quantity is identically **zero**, so the rule is
// uniform:
//
//   * wide  layout → the element's `uid:<uid>` column is absent
//     (a reader treats absence as 0);
//   * long  layout → no row is emitted for the (s, t, b) cell
//     (sums / min / max are invariant under dropped zeros).
//
// The tests below pin both halves: the structural LP-size saving
// (`get_numcols()` / `get_numrows()` diff between a baseline and a
// degenerate variant) AND, for two representative cases, the full
// build → solve → `write_out()` → read-back contract that the parquet
// stays self-consistent with the missing columns.
//
// Shortcuts covered:
//   1. ReservoirLP        — fmin == fmax == 0  → no extraction column
//   2. CommitmentLP       — generator elided every gen column → no u/v/w
//   3. ReserveZoneLP      — zero effective requirement → no requirement row
//   4. ReserveProvisionLP — rmax == rmin == 0   → no provision column
//   5. InertiaProvisionLP — provision_max == 0  → no provision column
//   6. InertiaZoneLP      — zero requirement     → no requirement row

#include <filesystem>

#include <doctest/doctest.h>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/commitment_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/waterway_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Uniquely-named outer namespace so helper symbols never collide with
// other test translation units under a CMake unity build.
namespace elim_shortcuts_test
{
namespace
{

// Two-block, one-stage, one-scenario simulation (non-chronological).
[[nodiscard]] Simulation make_two_block_sim(bool chronological = false)
{
  return {
      .block_array =
          {
              {
                  .uid = Uid {1},
                  .duration = 1.0,
              },
              {
                  .uid = Uid {2},
                  .duration = 2.0,
              },
          },
      .stage_array =
          {
              {
                  .uid = Uid {1},
                  .first_block = 0,
                  .count_block = 2,
                  .chronological = chronological,
              },
          },
      .scenario_array =
          {
              {
                  .uid = Uid {0},
              },
          },
  };
}

[[nodiscard]] std::filesystem::path leaf_parquet(
    const std::filesystem::path& dataset_dir)
{
  return dataset_dir / "scene=0" / "phase=0" / "part.parquet";
}

[[nodiscard]] std::filesystem::path leaf_parquet_stem(
    const std::filesystem::path& dataset_dir)
{
  return dataset_dir / "scene=0" / "phase=0" / "part";
}

[[nodiscard]] int find_column(const ArrowTable& table, std::string_view name)
{
  return table->schema()->GetFieldIndex(std::string {name});
}

[[nodiscard]] std::optional<std::vector<double>> read_double_column(
    const ArrowTable& table, std::string_view name)
{
  const int idx = find_column(table, name);
  if (idx < 0) {
    return std::nullopt;
  }
  const auto chunked = table->column(idx);
  std::vector<double> values;
  values.reserve(static_cast<std::size_t>(chunked->length()));
  for (int c = 0; c < chunked->num_chunks(); ++c) {
    const auto& chunk = chunked->chunk(c);
    const auto* arr =
        std::dynamic_pointer_cast<arrow::DoubleArray>(chunk).get();
    if (arr == nullptr) {
      return std::nullopt;
    }
    for (int64_t i = 0; i < arr->length(); ++i) {
      values.push_back(arr->IsNull(i) ? 0.0 : arr->Value(i));
    }
  }
  return values;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. ReservoirLP: fmin == fmax == 0 elides the extraction column.
// ---------------------------------------------------------------------------

TEST_CASE("LP-size: ReservoirLP skips zero-extraction blocks (fmin==fmax==0)")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 10.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const auto simulation = make_two_block_sim();

  auto solve = [&](double rsv_fmin, double rsv_fmax)
  {
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv1",
            .junction = Uid {1},
            .capacity = 1000.0,
            .emin = 0.0,
            .emax = 1000.0,
            .eini = 500.0,
            .fmin = rsv_fmin,
            .fmax = rsv_fmax,
        },
    };

    const System system = {
        .name = "ZeroExtractionTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    return lp.get_numcols();
  };

  const auto baseline_cols = solve(/*fmin=*/0.0, /*fmax=*/100.0);
  const auto zero_cols = solve(/*fmin=*/0.0, /*fmax=*/0.0);

  // The only structural difference is the two per-block extraction
  // columns — every storage-balance / energy column is identical.
  CHECK(baseline_cols - zero_cols == 2);
}

TEST_CASE(  // NOLINT
    "write-out: zero-extraction reservoir omits uid column in extraction_sol")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 10.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };

  const auto simulation = make_two_block_sim();

  auto solve_and_write =
      [&](double rsv_fmax, const std::filesystem::path& outdir)
  {
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv1",
            .junction = Uid {1},
            .capacity = 1000.0,
            .emin = 0.0,
            .emax = 1000.0,
            .eini = 500.0,
            .fmin = 0.0,
            .fmax = rsv_fmax,
        },
    };
    const System system = {
        .name = "ZeroExtractionParquet",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    popts.output_directory = outdir.string();
    popts.output_format = DataFormat::parquet;
    popts.output_layout = OutputLayout::wide;

    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    sys_lp.write_out();
  };

  const auto base_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_rsv_extract_base";
  const auto zero_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_rsv_extract_skip";
  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
  std::filesystem::create_directories(base_dir);
  std::filesystem::create_directories(zero_dir);

  solve_and_write(/*fmax=*/100.0, base_dir);
  solve_and_write(/*fmax=*/0.0, zero_dir);

  // Baseline: the extraction column for uid:1 is present (2 blocks).
  const auto base_dataset = base_dir / "Reservoir" / "extraction_sol.parquet";
  REQUIRE(std::filesystem::exists(leaf_parquet(base_dataset)));
  const auto base_table = parquet_read_table(leaf_parquet_stem(base_dataset));
  REQUIRE(base_table.has_value());
  const auto base_extract = read_double_column(*base_table, "uid:1");
  REQUIRE(base_extract.has_value());
  CHECK(base_extract->size() == 2);

  // Skip variant: extraction column entirely absent — the write-out
  // rule "elided extraction == 0" rendered as a missing uid column.
  // The dataset may not be written at all if it has no surviving cols.
  const auto zero_dataset = zero_dir / "Reservoir" / "extraction_sol.parquet";
  const auto zero_pq = leaf_parquet(zero_dataset);
  if (std::filesystem::exists(zero_pq)) {
    const auto zero_table = parquet_read_table(leaf_parquet_stem(zero_dataset));
    REQUIRE(zero_table.has_value());
    CHECK(find_column(*zero_table, "uid:1") < 0);
  }

  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
}

// ---------------------------------------------------------------------------
// 2. CommitmentLP: no generation columns → no orphan u/v/w binaries.
// ---------------------------------------------------------------------------

TEST_CASE("LP-size: CommitmentLP skips u/v/w when generator is fully OFF")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 20.0,
      },
  };

  const auto simulation = make_two_block_sim(/*chronological=*/true);

  auto build = [&](double uc_gen_capacity)
  {
    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "g_uc",
            .bus = Uid {1},
            .gcost = 50.0,
            .capacity = uc_gen_capacity,
        },
        {
            .uid = Uid {2},
            .name = "g_backup",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 1000.0,
        },
    };
    const Array<Commitment> commitment_array = {
        {
            .uid = Uid {1},
            .name = "g_uc_commit",
            .generator = Uid {1},
            .startup_cost = 100.0,
            .noload_cost = 5.0,
            .relax = true,
        },
    };
    const System system = {
        .name = "CommitmentOrphanTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .commitment_array = commitment_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    auto sim_lp = std::make_unique<SimulationLP>(simulation, options);
    auto sys_lp = std::make_unique<SystemLP>(system, *sim_lp);
    return std::pair {std::move(sim_lp), std::move(sys_lp)};
  };

  // Baseline: the committed generator has capacity → status cols exist.
  {
    auto [sim_lp, sys_lp] = build(/*uc_gen_capacity=*/100.0);
    const auto& cmt_lps = sys_lp->elements<CommitmentLP>();
    REQUIRE(cmt_lps.size() == 1);
    const auto& scenario_lp = sim_lp->scenarios().front();
    const auto& stage_lp = sim_lp->stages().front();
    const auto* status =
        cmt_lps.front().find_status_cols(scenario_lp, stage_lp);
    REQUIRE(status != nullptr);
    CHECK(status->size() == 2);  // one u per block
  }

  // Skip: capacity == 0 → generator elides every gen column → the
  // commitment returns early, creating no u/v/w binaries at all.
  {
    auto [sim_lp, sys_lp] = build(/*uc_gen_capacity=*/0.0);
    const auto& cmt_lps = sys_lp->elements<CommitmentLP>();
    REQUIRE(cmt_lps.size() == 1);
    const auto& scenario_lp = sim_lp->scenarios().front();
    const auto& stage_lp = sim_lp->stages().front();
    const auto* status =
        cmt_lps.front().find_status_cols(scenario_lp, stage_lp);
    CHECK(status == nullptr);  // write-out rule: status == 0 (OFF unit)
  }
}

TEST_CASE(  // NOLINT
    "write-out: fully-OFF committed generator omits status_sol uid column")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 20.0,
      },
  };

  const auto simulation = make_two_block_sim(/*chronological=*/true);

  auto solve_and_write =
      [&](double uc_gen_capacity, const std::filesystem::path& outdir)
  {
    const Array<Generator> generator_array = {
        {
            .uid = Uid {1},
            .name = "g_uc",
            .bus = Uid {1},
            .gcost = 50.0,
            .capacity = uc_gen_capacity,
        },
        {
            .uid = Uid {2},
            .name = "g_backup",
            .bus = Uid {1},
            .gcost = 100.0,
            .capacity = 1000.0,
        },
    };
    const Array<Commitment> commitment_array = {
        {
            .uid = Uid {1},
            .name = "g_uc_commit",
            .generator = Uid {1},
            .startup_cost = 100.0,
            .noload_cost = 5.0,
            .relax = true,
        },
    };
    const System system = {
        .name = "CommitmentOrphanParquet",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .commitment_array = commitment_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    popts.output_directory = outdir.string();
    popts.output_format = DataFormat::parquet;
    popts.output_layout = OutputLayout::wide;

    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    sys_lp.write_out();
  };

  const auto base_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_uc_status_base";
  const auto zero_dir =
      std::filesystem::temp_directory_path() / "gtopt_test_uc_status_skip";
  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
  std::filesystem::create_directories(base_dir);
  std::filesystem::create_directories(zero_dir);

  solve_and_write(/*uc_gen_capacity=*/100.0, base_dir);
  solve_and_write(/*uc_gen_capacity=*/0.0, zero_dir);

  // Baseline: Commitment/status_sol has the uid:1 status column.
  const auto base_dataset = base_dir / "Commitment" / "status_sol.parquet";
  REQUIRE(std::filesystem::exists(leaf_parquet(base_dataset)));
  const auto base_table = parquet_read_table(leaf_parquet_stem(base_dataset));
  REQUIRE(base_table.has_value());
  CHECK(find_column(*base_table, "uid:1") >= 0);

  // Skip: the orphan-binary elimination leaves no status column for the
  // OFF generator.  Dataset may be absent entirely (single commitment).
  const auto zero_dataset = zero_dir / "Commitment" / "status_sol.parquet";
  const auto zero_pq = leaf_parquet(zero_dataset);
  if (std::filesystem::exists(zero_pq)) {
    const auto zero_table = parquet_read_table(leaf_parquet_stem(zero_dataset));
    REQUIRE(zero_table.has_value());
    CHECK(find_column(*zero_table, "uid:1") < 0);
  }

  std::filesystem::remove_all(base_dir);
  std::filesystem::remove_all(zero_dir);
}

// ---------------------------------------------------------------------------
// 3. ReserveZoneLP: zero effective requirement → no `Σ pf·prov ≥ 0` row.
// ---------------------------------------------------------------------------

TEST_CASE("LP-size: ReserveZoneLP skips zero-requirement rows")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
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

  const auto simulation = make_two_block_sim();

  // Inspect the zone's own requirement-row holder rather than the global
  // LP row count: that isolates this shortcut from the (more intricate)
  // ReserveZone <-> ReserveProvision row coupling.
  auto req_row_count = [&](double urreq)
  {
    // Hard requirement branch: no urcost, no reserve_shortage_cost.
    const Array<ReserveZone> reserve_zone_array = {
        {
            .uid = Uid {1},
            .name = "rz1",
            .urreq = urreq,
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
    const System system = {
        .name = "ReserveZeroReqTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .reserve_zone_array = reserve_zone_array,
        .reserve_provision_array = reserve_provision_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);

    const auto& rz_lps = sys_lp.elements<ReserveZoneLP>();
    REQUIRE(rz_lps.size() == 1);
    const auto& scenario_lp = sim_lp.scenarios().front();
    const auto& stage_lp = sim_lp.stages().front();
    const auto& rows = rz_lps.front().urequirement_rows();
    const auto it = rows.find({scenario_lp.uid(), stage_lp.uid()});
    return it == rows.end() ? std::size_t {0} : it->second.size();
  };

  // urreq == 50 -> one requirement row per block (2); urreq == 0 -> none.
  CHECK(req_row_count(/*urreq=*/50.0) == 2);
  CHECK(req_row_count(/*urreq=*/0.0) == 0);
}

// ---------------------------------------------------------------------------
// 4. ReserveProvisionLP: rmax == rmin == 0 → no provision column.
// ---------------------------------------------------------------------------

TEST_CASE("LP-size: ReserveProvisionLP skips zero-bound provision columns")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
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

  const auto simulation = make_two_block_sim();

  auto build = [&](double urmax)
  {
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
            .urmax = urmax,
            .ur_provision_factor = 1.0,
        },
    };
    const System system = {
        .name = "ReserveZeroProvisionTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .reserve_zone_array = reserve_zone_array,
        .reserve_provision_array = reserve_provision_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    popts.model_options.reserve_shortage_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);
    auto&& lp = sys_lp.linear_interface();
    return lp.get_numcols();
  };

  const auto base_cols = build(/*urmax=*/100.0);
  const auto zero_cols = build(/*urmax=*/0.0);

  // One provision column per block disappears when urmax == urmin == 0.
  CHECK(base_cols - zero_cols == 2);
}

// ---------------------------------------------------------------------------
// 5. InertiaProvisionLP: provision_max == 0 → no provision column.
// ---------------------------------------------------------------------------

TEST_CASE("LP-size: InertiaProvisionLP skips zero-ceiling provision columns")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
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

  const auto simulation = make_two_block_sim();

  auto build = [&](double provision_max)
  {
    const Array<InertiaZone> inertia_zone_array = {
        {
            .uid = Uid {1},
            .name = "iz1",
            .requirement = 200.0,
            .cost = 1000.0,
        },
    };
    const Array<InertiaProvision> inertia_provision_array = {
        {
            .uid = Uid {1},
            .name = "ip1",
            .generator = Uid {1},
            .inertia_zones = {SingleId {Uid {1}}},
            .provision_max = provision_max,
            .provision_factor = 8.0,
        },
    };
    const System system = {
        .name = "InertiaZeroProvisionTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .inertia_zone_array = inertia_zone_array,
        .inertia_provision_array = inertia_provision_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);
    auto&& lp = sys_lp.linear_interface();
    return std::pair {lp.get_numcols(), lp.get_numrows()};
  };

  const auto [base_cols, base_rows] = build(/*provision_max=*/50.0);
  const auto [zero_cols, zero_rows] = build(/*provision_max=*/0.0);

  // Per block: one provision column AND one coupling row disappear.
  CHECK(base_cols - zero_cols == 2);
  CHECK(base_rows - zero_rows == 2);
}

// ---------------------------------------------------------------------------
// 6. InertiaZoneLP: zero requirement → no `Σ pf·r_inertia ≥ 0` row.
// ---------------------------------------------------------------------------

TEST_CASE("LP-size: InertiaZoneLP skips zero-requirement rows")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
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

  const auto simulation = make_two_block_sim();

  auto build_rows = [&](double requirement)
  {
    const Array<InertiaZone> inertia_zone_array = {
        {
            .uid = Uid {1},
            .name = "iz1",
            .requirement = requirement,
        },
    };
    const Array<InertiaProvision> inertia_provision_array = {
        {
            .uid = Uid {1},
            .name = "ip1",
            .generator = Uid {1},
            .inertia_zones = {SingleId {Uid {1}}},
            .provision_max = 50.0,
            .provision_factor = 8.0,
        },
    };
    const System system = {
        .name = "InertiaZeroReqTest",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .inertia_zone_array = inertia_zone_array,
        .inertia_provision_array = inertia_provision_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);
    return sys_lp.linear_interface().get_numrows();
  };

  const auto base_rows = build_rows(/*requirement=*/200.0);
  const auto zero_rows = build_rows(/*requirement=*/0.0);

  // One requirement row per block disappears when requirement == 0.
  CHECK(base_rows - zero_rows == 2);
}

// ---------------------------------------------------------------------------
// 7. AMPL / UserConstraint interface vs an eliminated column (silent-zero).
//
// The user's caution: a constraint that aggregates over an element's
// column — a flow-average / sum, or *any* AMPL reference that "requests a
// column value" — must keep working when that column was eliminated.
// The resolver's silent-zero contract (element_column_resolver.cpp,
// cases 1 & 2) contributes 0 for a registered-but-elided
// (scenario, stage, block) cell, which is exactly the fixed-zero value
// of every shortcut in this file.  Here a UserConstraint references the
// extraction of a reservoir whose extraction column is eliminated
// (fmin == fmax == 0): the system must still build (no strict-resolver
// error) AND solve, with the elided term contributing 0 to the LHS.
// ---------------------------------------------------------------------------

TEST_CASE(  // NOLINT
    "AMPL: UserConstraint over an eliminated reservoir extraction silent-zeros")
{
  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 10.0,
      },
  };
  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_backup",
          .bus = Uid {1},
          .gcost = 100.0,
          .capacity = 1000.0,
      },
  };
  const Array<Junction> junction_array = {
      {
          .uid = Uid {1},
          .name = "j_up",
      },
      {
          .uid = Uid {2},
          .name = "j_down",
          .drain = true,
      },
  };
  const Array<Waterway> waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = 0.0,
          .fmax = 100.0,
      },
  };
  // The aggregating constraint that "requests a column value" from the
  // (possibly eliminated) reservoir extraction.
  const Array<UserConstraint> user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "extraction_cap",
          .expression = "reservoir('rsv1').extraction <= 50, "
                        "for(stage in all, block in all)",
      },
  };

  const auto simulation = make_two_block_sim();

  auto build_and_solve = [&](double rsv_fmax)
  {
    const Array<Reservoir> reservoir_array = {
        {
            .uid = Uid {1},
            .name = "rsv1",
            .junction = Uid {1},
            .capacity = 1000.0,
            .emin = 0.0,
            .emax = 1000.0,
            .eini = 500.0,
            .fmin = 0.0,
            .fmax = rsv_fmax,
        },
    };
    const System system = {
        .name = "ExtractionUserConstraint",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .junction_array = junction_array,
        .waterway_array = waterway_array,
        .reservoir_array = reservoir_array,
        .user_constraint_array = user_constraint_array,
    };

    PlanningOptions popts;
    popts.model_options.demand_fail_cost = 10000.0;
    const PlanningOptionsLP options(popts);
    SimulationLP sim_lp(simulation, options);

    // The strict resolver must NOT raise on the eliminated-extraction
    // reference — it silent-zeros instead.
    REQUIRE_NOTHROW(SystemLP {system, sim_lp});
    SystemLP sys_lp(system, sim_lp);
    auto&& lp = sys_lp.linear_interface();
    const auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
  };

  // Extraction column present (fmax = 100): the UC references a live col.
  build_and_solve(/*rsv_fmax=*/100.0);
  // Extraction column ELIMINATED (fmin == fmax == 0): the UC term
  // silent-zeros (`0 <= 50`), the build does not throw, and the LP
  // still solves with the thermal backup carrying the load.
  build_and_solve(/*rsv_fmax=*/0.0);
}

}  // namespace elim_shortcuts_test
