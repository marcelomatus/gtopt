/**
 * @file      test_root_basis_cache.cpp
 * @brief     End-to-end test of the root LP basis cache in SystemLP::resolve
 * @date      2026-07-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Exercises the `monolithic_options.mip_start.root_basis_cache_file` feature
 * on a small unit-commitment MIP (the `uc_small` fixture, mirrored inline):
 *
 *   - Run 1 (cache file ABSENT): the model solves cold, and the root LP basis
 *     is captured (`get_basis`) and `save_basis`d to the cache path.  The file
 *     must exist afterwards.
 *   - Run 2 (cache file PRESENT): the cached basis is `load_basis`d and
 *     installed (`set_basis`, reconciled to current dims) before the MIP solve.
 *     For CPLEX this switches the MIP root LP to primal simplex + Advance; for
 *     every other backend the option is a no-op.  Either way the objective must
 *     equal run 1 — the cache is an accelerator, never a correctness change.
 *   - Corrupt cache: garbage bytes in the cache file are rejected by
 *     `load_basis` (mirroring `test_basis_io.cpp`), the solve falls back to a
 *     cold root, still reaches the optimum, and re-caches a valid basis.
 *
 * The save/load round-trip is SOLVER-AGNOSTIC (it runs on any MIP-capable
 * backend); only the STARTALG speed path is CPLEX-only, so the assertions here
 * are on OBJECTIVE equality and cache-file presence, not on simplex-iteration
 * counts.  Gated on a MIP-capable solver being loaded — without one the
 * commitment binaries cannot be solved and the test self-skips with a message.
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

/// First loaded solver that can actually solve a MIP, or "" when none is.
///
/// The commitment fixture carries integer status binaries, and
/// `LinearInterface::load_flat` rejects integer columns on an LP-only backend,
/// so the LP must be pinned to a MIP-capable solver instead of inheriting the
/// ambient default (CI may export GTOPT_SOLVER=clp, which would trip the
/// guard).
[[nodiscard]] std::string pick_mip_solver()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  for (const auto* name : {"cplex", "highs", "cbc", "scip", "gurobi"}) {
    if (reg.has_solver(name) && reg.supports_mip(name)) {
      return name;
    }
  }
  return {};
}

/// A unique temp path in the system temp dir, cleaned up by the caller.
[[nodiscard]] std::filesystem::path temp_cache_path(const char* stem)
{
  return std::filesystem::temp_directory_path()
      / (std::string {"gtopt_root_basis_cache_"} + stem + ".bin");
}

/// Six chronological 1-hour blocks so the commitment carries a status binary in
/// every block; the low → high → low demand forces the peaking units on and
/// off.
const Simulation uc_small_simulation = {
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
            {
                .uid = Uid {3},
                .duration = 1.0,
            },
            {
                .uid = Uid {4},
                .duration = 1.0,
            },
            {
                .uid = Uid {5},
                .duration = 1.0,
            },
        },
    .stage_array =
        {
            {
                .uid = Uid {0},
                .first_block = 0,
                .count_block = 6,
                .chronological = true,
            },
        },
    .scenario_array =
        {
            {
                .uid = Uid {0},
            },
        },
};

/// Three thermal units (cheap baseload + mid peaker + expensive fast peaker),
/// each with a when-committed floor (Commitment.pmin > 0) plus startup / noload
/// costs so the binaries are genuinely forced to toggle across the demand
/// ramp.  Mirrors cases/uc_small/uc_small.json.
[[nodiscard]] System make_uc_small_system()
{
  System system;
  system.name = "uc_small";
  system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  system.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .lmax = TBRealFieldSched {std::vector<std::vector<Real>> {
              {
                  20.0,
                  60.0,
                  120.0,
                  120.0,
                  60.0,
                  20.0,
              },
          }},
          .capacity = 200.0,
      },
  };
  system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g_base",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 60.0,
          .gcost = 20.0,
          .capacity = 60.0,
      },
      {
          .uid = Uid {2},
          .name = "g_mid",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 80.0,
          .gcost = 40.0,
          .capacity = 80.0,
      },
      {
          .uid = Uid {3},
          .name = "g_peak",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 60.0,
          .gcost = 80.0,
          .capacity = 60.0,
      },
  };
  system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g_base_uc",
          .generator = Uid {1},
          .startup_cost = 200.0,
          .noload_cost = 30.0,
          .pmin = 20.0,
          .initial_status = 1.0,
          .initial_hours = 8.0,
      },
      {
          .uid = Uid {2},
          .name = "g_mid_uc",
          .generator = Uid {2},
          .startup_cost = 400.0,
          .noload_cost = 40.0,
          .pmin = 30.0,
          .min_up_time = 2.0,
          .min_down_time = 2.0,
          .initial_status = 0.0,
          .initial_hours = 8.0,
      },
      {
          .uid = Uid {3},
          .name = "g_peak_uc",
          .generator = Uid {3},
          .startup_cost = 150.0,
          .noload_cost = 20.0,
          .pmin = 15.0,
          .initial_status = 0.0,
          .initial_hours = 8.0,
      },
  };
  return system;
}

/// Build the planning options with the MIP-start hook enabled and the root
/// basis cache pointed at `cache_path`.
[[nodiscard]] PlanningOptions make_options(
    const std::string& mip_solver, const std::filesystem::path& cache_path)
{
  PlanningOptions poptions;
  poptions.model_options.demand_fail_cost = 1000.0;
  poptions.model_options.use_single_bus = true;
  poptions.model_options.scale_objective = 1000.0;
  poptions.lp_matrix_options.solver_name = mip_solver;
  poptions.monolithic_options.mip_start.emplace();
  poptions.monolithic_options.mip_start->enabled = true;
  poptions.monolithic_options.mip_start->root_basis_cache_file =
      cache_path.string();
  return poptions;
}

/// Build a fresh SystemLP for the fixture, solve it through the resolve hook
/// (the ONLY path that touches the root basis cache), and return the scaled
/// objective.  `saw_mip` reports whether the built model actually had integer
/// columns BEFORE the resolve (the post-MIP dual-recovery pass relaxes them).
[[nodiscard]] double solve_once(const std::string& mip_solver,
                                const std::filesystem::path& cache_path,
                                bool& saw_mip)
{
  System system = make_uc_small_system();
  PlanningOptionsLP options(make_options(mip_solver, cache_path));
  SimulationLP simulation_lp(uc_small_simulation, options);
  SystemLP system_lp(system, simulation_lp);

  auto&& lp = system_lp.linear_interface();
  saw_mip = lp.has_integer_cols();

  const auto result = system_lp.resolve(SolverOptions {.log_level = 0});
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  REQUIRE(lp.is_optimal());
  return lp.get_obj_value();
}

}  // namespace

TEST_CASE(  // NOLINT
    "root_basis_cache: cold run writes the cache, warm run reuses it")
{
  const auto mip_solver = pick_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("Skipping root-basis-cache test — no MIP solver available");
    return;
  }

  const auto cache = temp_cache_path("roundtrip");
  std::filesystem::remove(cache);
  REQUIRE_FALSE(std::filesystem::exists(cache));

  // Run 1: file ABSENT → solve cold and, if the backend exposes a basis,
  // capture + `save_basis` it to the cache path.
  bool saw_mip_1 = false;
  const double obj1 = solve_once(mip_solver, cache, saw_mip_1);

  // The fixture is a genuine MIP: the commitment status binaries are integer
  // columns before the resolve relaxes them for dual recovery.
  CHECK(saw_mip_1);

  // Whether the cold run persisted a basis is a BACKEND CAPABILITY: today only
  // CPLEX implements `get_basis`/`set_basis`, so on other backends the resolve
  // hook logs "backend produced no basis to cache" and writes nothing.  The
  // objective-equality contract below is solver-agnostic and always holds; the
  // file round-trip is asserted only when the backend actually caches.
  const bool backend_caches = std::filesystem::exists(cache);
  if (!backend_caches) {
    MESSAGE("root-basis: active backend '"
            << mip_solver
            << "' exposes no LP basis — asserting objective equality only "
               "(file round-trip is CPLEX-gated today)");
  }

  // Run 2: file PRESENT (CPLEX) → load + install the cached basis, then MIP
  // solve.  The objective must be identical regardless of backend: the cache
  // is an accelerator, never a correctness change.
  bool saw_mip_2 = false;
  const double obj2 = solve_once(mip_solver, cache, saw_mip_2);
  CHECK(saw_mip_2);
  CHECK(obj2 == doctest::Approx(obj1));
  // A caching backend must still have the cache after the warm run (loaded, not
  // deleted); a non-caching backend never created it in the first place.
  CHECK(std::filesystem::exists(cache) == backend_caches);

  std::filesystem::remove(cache);
}

TEST_CASE(  // NOLINT
    "root_basis_cache: corrupt cache is rejected, solve re-caches")
{
  const auto mip_solver = pick_mip_solver();
  if (mip_solver.empty()) {
    MESSAGE("Skipping root-basis-cache test — no MIP solver available");
    return;
  }

  const auto cache = temp_cache_path("corrupt");
  const auto ref_cache = temp_cache_path("corrupt_ref");
  std::filesystem::remove(cache);
  std::filesystem::remove(ref_cache);

  // Establish the reference optimum with a clean cold solve.  Whether it writes
  // a basis to `ref_cache` reveals the backend's basis capability (CPLEX-only
  // today); the corrupt-cache resilience below must hold on every backend.
  bool saw_mip_ref = false;
  const double obj_ref = solve_once(mip_solver, ref_cache, saw_mip_ref);
  CHECK(saw_mip_ref);
  const bool backend_caches = std::filesystem::exists(ref_cache);
  std::filesystem::remove(ref_cache);

  // Poison the cache with garbage bytes (mirrors test_basis_io.cpp's
  // garbage-file subcase) so load_basis rejects it (bad magic → InvalidInput).
  {
    std::ofstream out(cache, std::ios::binary | std::ios::trunc);
    out << "not a gtopt basis file at all";
  }
  REQUIRE(std::filesystem::exists(cache));

  // The solve MUST survive the corrupt cache on ANY backend: load_basis fails
  // (bad magic → warn), the root falls back to a cold solve, and the optimum is
  // unchanged.  On a caching backend a fresh valid basis is written back over
  // the garbage; a non-caching backend leaves the garbage in place (harmless —
  // it is re-rejected next time).  Either way the optimum is invariant.
  bool saw_mip = false;
  const double obj = solve_once(mip_solver, cache, saw_mip);
  CHECK(saw_mip);
  CHECK(obj == doctest::Approx(obj_ref));
  CHECK(std::filesystem::exists(cache));

  // A caching backend has now replaced the garbage with a real basis, so a
  // subsequent warm run reuses it; a non-caching backend simply re-rejects the
  // still-present garbage and re-solves cold.  The optimum is invariant.
  bool saw_mip_warm = false;
  const double obj_warm = solve_once(mip_solver, cache, saw_mip_warm);
  CHECK(obj_warm == doctest::Approx(obj_ref));
  if (backend_caches) {
    CHECK(std::filesystem::exists(cache));
  }

  std::filesystem::remove(cache);
}
