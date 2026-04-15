/**
 * @file      test_mip_solvers.cpp
 * @brief     MIP tests exercised across every available MIP-capable solver
 * @date      2026-04-15
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Runs three MIP test scenarios (integer expansion, unit commitment,
 * simple commitment) for every solver plugin that reports supports_mip().
 * Solvers that lack MIP support (e.g. CLP) are silently skipped.
 */

#include <format>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/commitment_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simple_commitment.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Return the list of loaded solvers that support MIP.
[[nodiscard]] std::vector<std::string> mip_solvers()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  std::vector<std::string> result;
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      result.push_back(name);
    }
  }
  return result;
}

// ── Shared fixtures ─────────────────────────────────────────────────────────

const Array<Bus> single_bus = {{
    .uid = Uid {1},
    .name = "b1",
}};

const Simulation single_block_simulation = {
    .block_array = {{
        .uid = Uid {1},
        .duration = 1,
    }},
    .stage_array = {{
        .uid = Uid {1},
        .first_block = 0,
        .count_block = 1,
    }},
    .scenario_array = {{
        .uid = Uid {0},
    }},
};

const Simulation three_block_simulation = {
    .block_array =
        {
            {
                .uid = Uid {0},
                .duration = 1.0,
            },
            {
                .uid = Uid {1},
                .duration = 1.0,
            },
            {
                .uid = Uid {2},
                .duration = 1.0,
            },
        },
    .stage_array = {{
        .uid = Uid {0},
        .first_block = 0,
        .count_block = 3,
        .chronological = true,
    }},
    .scenario_array = {{
        .uid = Uid {0},
    }},
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1. Integer expansion modules — all MIP solvers
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("MIP solvers — integer_expmod gives integer expansion modules")
{
  const auto solvers = mip_solvers();
  if (solvers.empty()) {
    MESSAGE("No MIP-capable solver available — skipping");
    return;
  }

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(solver_name.c_str())
    {
      const Array<Generator> generator_array = {{
          .uid = Uid {1},
          .name = "g_int",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 0.0,
          .expcap = 30.0,
          .expmod = 10.0,
          .annual_capcost = 1000.0,
          .integer_expmod = true,
      }};

      const Array<Demand> demand_array = {{
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 75.0,
      }};

      const System system = {
          .name = "MipExpmod_" + solver_name,
          .bus_array = single_bus,
          .demand_array = demand_array,
          .generator_array = generator_array,
      };

      PlanningOptions popts;
      popts.model_options.demand_fail_cost = 10000.0;
      const PlanningOptionsLP options(popts);
      SimulationLP simulation_lp(single_block_simulation, options);

      LpMatrixOptions flat_opts;
      flat_opts.solver_name = solver_name;
      SystemLP system_lp(system, simulation_lp, flat_opts);

      auto&& lp = system_lp.linear_interface();

      const auto& gen_lps = system_lp.elements<GeneratorLP>();
      REQUIRE(gen_lps.size() == 1);
      const auto& gen_lp = gen_lps.front();

      const auto& stage_lp = simulation_lp.stages().front();
      const auto expmod_col = gen_lp.expmod_col_at(stage_lp);
      REQUIRE(expmod_col.has_value());
      CHECK(lp.is_integer(*expmod_col));

      auto result = lp.resolve();
      REQUIRE(result.has_value());
      CHECK(result.value() == 0);

      const auto col_sol = lp.get_col_sol();
      CHECK(col_sol[*expmod_col] == doctest::Approx(3.0).epsilon(1e-6));

      const auto cap_col = gen_lp.capacity_col_at(stage_lp);
      REQUIRE(cap_col.has_value());
      CHECK(col_sol[*cap_col] == doctest::Approx(90.0).epsilon(1e-6));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Commitment MIP u/v/w binary values — all MIP solvers
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "MIP solvers — commitment u/v/w binary values for startup profile")
{
  const auto solvers = mip_solvers();
  if (solvers.empty()) {
    MESSAGE("No MIP-capable solver available — skipping");
    return;
  }

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(solver_name.c_str())
    {
      System system;
      system.name = "MipCommitment_" + solver_name;
      system.bus_array = single_bus;
      system.demand_array = {{
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = TBRealFieldSched {std::vector<std::vector<Real>> {{
              0.0,
              60.0,
              60.0,
          }}},
          .capacity = 100.0,
      }};
      system.generator_array = {{
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 30.0,
          .pmax = 100.0,
          .gcost = 50.0,
          .capacity = 100.0,
      }};
      system.commitment_array = {{
          .uid = Uid {1},
          .name = "cmt1",
          .generator = Uid {1},
          .startup_cost = 100.0,
          .shutdown_cost = 50.0,
          .initial_status = 0.0,
      }};

      PlanningOptions poptions;
      poptions.model_options.demand_fail_cost = 1000.0;
      poptions.use_single_bus = true;
      poptions.lp_matrix_options.col_with_names = true;
      poptions.lp_matrix_options.col_with_name_map = true;
      poptions.lp_matrix_options.solver_name = solver_name;
      PlanningOptionsLP options(std::move(poptions));

      SimulationLP simulation_lp(three_block_simulation, options);

      LpMatrixOptions flat_opts;
      flat_opts.col_with_names = true;
      flat_opts.col_with_name_map = true;
      flat_opts.solver_name = solver_name;
      SystemLP system_lp(system, simulation_lp, flat_opts);

      auto&& lp = system_lp.linear_interface();

      const auto result = lp.resolve();
      REQUIRE(result.has_value());
      CHECK(result.value() == 0);

      const auto& col_map = lp.col_name_map();
      const auto sol = lp.get_col_sol();

      auto find_col = [&](std::string_view variable,
                          Uid block_uid) -> std::optional<ColIndex>
      {
        for (const auto& [name, idx] : col_map) {
          if (name.find("commitment_") == 0
              && name.find(std::string(variable) + "_") != std::string::npos
              && name.size() >= 2
              && name.substr(name.size() - 2)
                  == std::string("_") + std::to_string(block_uid))
          {
            return idx;
          }
        }
        return std::nullopt;
      };

      const auto u0 = find_col(CommitmentLP::StatusName, Uid {0});
      const auto u1 = find_col(CommitmentLP::StatusName, Uid {1});
      const auto u2 = find_col(CommitmentLP::StatusName, Uid {2});
      const auto v1 = find_col(CommitmentLP::StartupName, Uid {1});
      const auto v2 = find_col(CommitmentLP::StartupName, Uid {2});
      const auto w2 = find_col(CommitmentLP::ShutdownName, Uid {2});

      REQUIRE(u0.has_value());
      REQUIRE(u1.has_value());
      REQUIRE(u2.has_value());
      REQUIRE(v1.has_value());
      REQUIRE(v2.has_value());
      REQUIRE(w2.has_value());

      CHECK(lp.is_integer(*u0));
      CHECK(lp.is_integer(*u1));
      CHECK(lp.is_integer(*u2));
      CHECK(lp.is_integer(*v1));
      CHECK(lp.is_integer(*v2));
      CHECK(lp.is_integer(*w2));

      CHECK(sol[*u0] == doctest::Approx(0.0).epsilon(1e-4));
      CHECK(sol[*u1] == doctest::Approx(1.0).epsilon(1e-4));
      CHECK(sol[*u2] == doctest::Approx(1.0).epsilon(1e-4));
      CHECK(sol[*v1] == doctest::Approx(1.0).epsilon(1e-4));
      CHECK(sol[*v2] == doctest::Approx(0.0).epsilon(1e-4));
      CHECK(sol[*w2] == doctest::Approx(0.0).epsilon(1e-4));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Simple commitment MIP — all MIP solvers
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE(  // NOLINT
    "MIP solvers — simple commitment status u binary when pmin exceeds demand")
{
  const auto solvers = mip_solvers();
  if (solvers.empty()) {
    MESSAGE("No MIP-capable solver available — skipping");
    return;
  }

  for (const auto& solver_name : solvers) {
    CAPTURE(solver_name);

    SUBCASE(solver_name.c_str())
    {
      const Array<Demand> small_demand_array = {{
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 20.0,
      }};

      const Array<Generator> generator_array = {{
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 40.0,
          .gcost = 50.0,
          .capacity = 100.0,
      }};

      const Array<SimpleCommitment> simple_commitment_array = {{
          .uid = Uid {1},
          .name = "sc1",
          .generator = Uid {1},
          .dispatch_pmin = 40.0,
          .relax = false,
      }};

      PlanningOptions opts;
      opts.demand_fail_cost = 1000.0;
      opts.lp_matrix_options.solver_name = solver_name;

      const System system = {
          .name = "MipSimpleCommit_" + solver_name,
          .bus_array = single_bus,
          .demand_array = small_demand_array,
          .generator_array = generator_array,
          .simple_commitment_array = simple_commitment_array,
      };

      const PlanningOptionsLP options(opts);
      SimulationLP simulation_lp(single_block_simulation, options);
      SystemLP system_lp(system, simulation_lp);

      auto&& lp = system_lp.linear_interface();

      const auto& sc_lps = system_lp.elements<SimpleCommitmentLP>();
      REQUIRE(sc_lps.size() == 1);
      const auto& sc_lp = sc_lps.front();
      const auto& scenario_lp = simulation_lp.scenarios().front();
      const auto& stage_lp = simulation_lp.stages().front();
      const auto& block_lp = simulation_lp.blocks().front();
      const auto u_col =
          sc_lp.lookup_status_col(scenario_lp, stage_lp, block_lp.uid());
      REQUIRE(u_col.has_value());
      CHECK(lp.is_integer(*u_col));

      auto result = lp.resolve();
      REQUIRE(result.has_value());
      CHECK(result.value() == 0);

      CHECK(lp.get_col_sol()[*u_col] == doctest::Approx(0.0).epsilon(0.001));
    }
  }
}
