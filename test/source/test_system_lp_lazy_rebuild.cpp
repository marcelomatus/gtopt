// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_system_lp_lazy_rebuild.cpp
 * @brief     Lock-in tests for the disposable-collection drop + two-flag
 *            rebuild gate added to SystemLP under LowMemoryMode::compress.
 * @date      2026-05-05
 * @copyright BSD-3-Clause
 *
 * Three groups of contracts pinned here:
 *
 *   C. `is_post_add_to_lp_resident_v<T>` trait — every `HasUpdateLP` type
 *      stays alive after `add_to_lp` (collection survives compress drop);
 *      every other element type is disposable.
 *
 *   D. `clear_disposable_collections()` runtime behavior — under
 *      `LowMemoryMode::compress` the `PlanningLP` ctor flow drops every
 *      disposable collection.  After construction `elements<GeneratorLP>()`
 *      / `elements<ReservoirLP>()` are empty, while the resident
 *      `HasUpdateLP` collections (e.g. `FlowRightLP`) still respond to
 *      reads.
 *
 *   E. Two-flag rebuild gate — `update_lp()` (which goes through
 *      `ensure_lp_built()` / `ensure_backend()` but NOT
 *      `rebuild_collections_if_needed`) must NOT rehydrate the disposable
 *      collections.  `rebuild_collections_if_needed()` is the only
 *      sanctioned trigger and is idempotent on subsequent calls.
 */

#include <doctest/doctest.h>
#include <gtopt/bus_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/reservoir_discharge_limit_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_production_factor_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/volume_right_lp.hpp>

#include "fixture_helpers.hpp"

using namespace gtopt;
using gtopt::test_fixtures::make_single_stage_phases;
using gtopt::test_fixtures::make_uniform_blocks;
using gtopt::test_fixtures::make_uniform_stages;

// ─── C. is_post_add_to_lp_resident_v compile-time trait ─────────────────────

// Resident: every HasUpdateLP type must stay alive across SDDP iterations
// because update_lp() depends on per-(scen, stg) state stored in the
// element wrappers.
static_assert(is_post_add_to_lp_resident_v<FlowRightLP>);
static_assert(is_post_add_to_lp_resident_v<ReservoirSeepageLP>);
static_assert(is_post_add_to_lp_resident_v<ReservoirDischargeLimitLP>);
static_assert(is_post_add_to_lp_resident_v<ReservoirProductionFactorLP>);
static_assert(is_post_add_to_lp_resident_v<VolumeRightLP>);

// Disposable: pure add_to_lp consumers — everything they contributed has
// already landed in the LP snapshot, so the collection is dead weight
// after construction.
static_assert(!is_post_add_to_lp_resident_v<GeneratorLP>);
static_assert(!is_post_add_to_lp_resident_v<DemandLP>);
static_assert(!is_post_add_to_lp_resident_v<BusLP>);
// IMPORTANT: ReservoirLP is now disposable.  Phase 2a's StateVariable
// channel and the daily-cycle short-circuit removed every
// `prev_sys->element<ReservoirLP>` lookup from update_lp's hot path.
static_assert(!is_post_add_to_lp_resident_v<ReservoirLP>);

// One runtime TEST_CASE so doctest reports the trait verifications as a
// passing test (the `static_assert`s above already gate compilation).
TEST_CASE(
    "is_post_add_to_lp_resident_v: compile-time trait verified")  // NOLINT
{
  CHECK(is_post_add_to_lp_resident_v<FlowRightLP>);
  CHECK(is_post_add_to_lp_resident_v<ReservoirSeepageLP>);
  CHECK(is_post_add_to_lp_resident_v<ReservoirDischargeLimitLP>);
  CHECK(is_post_add_to_lp_resident_v<ReservoirProductionFactorLP>);
  CHECK(is_post_add_to_lp_resident_v<VolumeRightLP>);

  CHECK_FALSE(is_post_add_to_lp_resident_v<GeneratorLP>);
  CHECK_FALSE(is_post_add_to_lp_resident_v<DemandLP>);
  CHECK_FALSE(is_post_add_to_lp_resident_v<BusLP>);
  CHECK_FALSE(is_post_add_to_lp_resident_v<ReservoirLP>);
}

// ─── D / E. Runtime behavior fixture ────────────────────────────────────────
//
// Tiny single-phase, single-stage planning fixture: 1 bus, 1 demand,
// 1 generator, 1 reservoir + 1 waterway / turbine pair so the reservoir
// is structurally connected.  The shape mirrors the
// `make_two_phase_reservoir_planning` helper in
// `test_update_context.cpp` but trimmed to a single phase / scene for
// fast construction in the compress-mode tests below.

namespace
{

[[nodiscard]] inline auto make_single_phase_reservoir_planning() -> Planning
{
  Array<Block> block_array = make_uniform_blocks(1, 1.0);
  Array<Stage> stage_array = make_uniform_stages(1, 1);
  Array<Phase> phase_array = make_single_stage_phases(1);

  const Array<Bus> bus_array = {
      {
          .uid = Uid {1},
          .name = "bus1",
      },
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "thermal_gen",
          .bus = Uid {1},
          .gcost = 50.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "load1",
          .bus = Uid {1},
          .capacity = 100.0,
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

  const Array<Reservoir> reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {1},
          .capacity = 500.0,
          .emin = 0.0,
          .emax = 500.0,
          .eini = 100.0,
          .fmin = -1000.0,
          .fmax = +1000.0,
          .flow_conversion_rate = 1.0,
          .use_state_variable = true,
          .daily_cycle = false,
      },
  };

  Simulation simulation = {
      .block_array = std::move(block_array),
      .stage_array = std::move(stage_array),
      .scenario_array =
          {
              {
                  .uid = Uid {1},
              },
          },
      .phase_array = std::move(phase_array),
  };

  PlanningOptions options;
  options.model_options.demand_fail_cost = 1000.0;
  options.model_options.use_single_bus = OptBool {true};
  options.model_options.scale_objective = OptReal {1.0};
  // The PlanningLP ctor forces low_memory_mode = off for the
  // monolithic method (single solve per cell, no amortisation), which
  // would skip `clear_disposable_collections()` and break every
  // compress-mode test below.  Pin the method to sddp so the requested
  // compress mode actually fires.  Single-phase planning is fine here
  // — only the planner falls back to monolithic on <2 phases; the LP
  // builder honours the requested mode regardless.
  options.method = MethodType::sddp;

  System system = {
      .name = "single_phase_reservoir",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .junction_array = junction_array,
      .waterway_array = waterway_array,
      .reservoir_array = reservoir_array,
  };

  return Planning {
      .options = std::move(options),
      .simulation = std::move(simulation),
      .system = std::move(system),
  };
}

}  // namespace

// ─── D. clear_disposable_collections runtime behavior ───────────────────────

TEST_CASE(  // NOLINT
    "SystemLP under compress: ctor drops disposable collections, keeps "
    "HasUpdateLP types alive")
{
  // Build a tiny PlanningLP under LowMemoryMode::compress.  The PlanningLP
  // ctor invokes `clear_disposable_collections()` after
  // `tighten_scene_phase_links` finishes, dropping every collection
  // whose element type does NOT satisfy HasUpdateLP.
  LpMatrixOptions flat_opts;
  flat_opts.low_memory_mode = LowMemoryMode::compress;

  PlanningLP planning_lp(make_single_phase_reservoir_planning(), flat_opts);

  auto& sys = planning_lp.system(first_scene_index(), first_phase_index());

  SUBCASE("HasUpdateLP collection (FlowRightLP) is queryable post-ctor")
  {
    // FlowRightLP is a HasUpdateLP type — its collection MUST stay live
    // even though the JSON has zero flow_right entries.  The assertion
    // is that `elements<FlowRightLP>()` does NOT throw the
    // "empty collections" runtime_error: an empty range is fine, an
    // empty *collection-state* (m_collections_built_ = false) is not.
    const auto& flow_rights = sys.elements<FlowRightLP>();
    CHECK(flow_rights.empty());  // No FlowRights in this fixture.
  }

  SUBCASE("Disposable collection (GeneratorLP) is empty post-ctor")
  {
    // The fixture has 1 generator, but `clear_disposable_collections()`
    // ran during the ctor — the collection is empty even though the
    // System data still has the generator.
    const auto& generators = sys.elements<GeneratorLP>();
    CHECK(generators.empty());
  }

  SUBCASE("Disposable collection (ReservoirLP) is empty post-ctor")
  {
    // ReservoirLP is now disposable too (since the StateVariable channel
    // landed) — same drop applies, even though the fixture has 1
    // reservoir.
    static_assert(!HasUpdateLP<ReservoirLP>,
                  "ReservoirLP must NOT satisfy HasUpdateLP for "
                  "clear_disposable_collections() to drop it");
    const auto& reservoirs = sys.elements<ReservoirLP>();
    CHECK(reservoirs.empty());
  }
}

// ─── E. Two-flag rebuild gate ───────────────────────────────────────────────

TEST_CASE(  // NOLINT
    "SystemLP under compress: update_lp does NOT rehydrate disposable "
    "collections; rebuild_collections_if_needed does")
{
  LpMatrixOptions flat_opts;
  flat_opts.low_memory_mode = LowMemoryMode::compress;

  PlanningLP planning_lp(make_single_phase_reservoir_planning(), flat_opts);

  auto& sys = planning_lp.system(first_scene_index(), first_phase_index());

  // Initial state after PlanningLP ctor: disposable types dropped.
  REQUIRE(sys.elements<GeneratorLP>().empty());
  REQUIRE(sys.elements<ReservoirLP>().empty());

  SUBCASE("update_lp() leaves disposable collections empty")
  {
    // update_lp deliberately skips `rebuild_collections_if_needed` —
    // its `visit_elements` only acts on HasUpdateLP types (which are
    // alive) and rehydrating the disposable types every iteration
    // would burn the very allocation the compress mode is trying to
    // amortise away.
    const auto modifications = sys.update_lp();
    // Tiny fixture has no per-(scen, stg) update side-effect, but the
    // call must not crash and must not flip the disposable flag.
    CHECK(modifications >= 0);
    CHECK(sys.elements<GeneratorLP>().empty());
    CHECK(sys.elements<ReservoirLP>().empty());
  }

  SUBCASE("rebuild_collections_if_needed() rehydrates and is idempotent")
  {
    // First call: rebuild fires the create_collections + throwaway
    // flatten path, repopulating every disposable type.
    sys.rebuild_collections_if_needed();
    CHECK_FALSE(sys.elements<GeneratorLP>().empty());
    CHECK_FALSE(sys.elements<ReservoirLP>().empty());
    const auto gen_count = sys.elements<GeneratorLP>().size();
    const auto rsv_count = sys.elements<ReservoirLP>().size();
    CHECK(gen_count == 1);
    CHECK(rsv_count == 1);

    // Second call: two-flag gate (m_collections_built_ &&
    // m_disposable_collections_built_) must short-circuit — the
    // collection contents stay identical, no spurious rebuild.
    sys.rebuild_collections_if_needed();
    CHECK(sys.elements<GeneratorLP>().size() == gen_count);
    CHECK(sys.elements<ReservoirLP>().size() == rsv_count);
  }
}
