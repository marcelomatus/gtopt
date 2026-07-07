// SPDX-License-Identifier: BSD-3-Clause
/// @file test_user_constraint_resolver_leniency.cpp
/// @brief Pin the strict-mode resolver's "element registered for some
///        (scenario, stage, block) but not this one" tolerance.
///
/// Regression guard for the user-constraint resolver fix landed
/// while diagnosing CEN PCP weekly LNG +229% over-dispatch
/// (plexos2gtopt's ``FueMaxOff_<fuel>`` expressions include
/// generators whose per-block ``pmax`` profile drops to 0 in some
/// blocks — startup-staged or alternate-fuel-mode variants).
///
/// Behaviour pinned:
///
///   * Element is registered as an LP variable somewhere AND has no
///     LP column for the specific (scenario, stage, block) being
///     evaluated → the term silently contributes 0 to the LHS.  No
///     strict-mode error, no warning at INFO level.
///   * Element name or attribute is unknown to the AMPL registry
///     entirely (typo, unregistered) → strict-mode error preserved.
///     The complementary tests in ``test_line_ampl_compound.cpp``
///     ("lossless: direct ``flown`` reference throws in strict mode"
///     and "AMPL capacity — capacity-attr typo still throws ...")
///     already pin the strict-throw side; this file pins the
///     silent-skip side.
///
/// See ``element_column_resolver.cpp::stamp_ref`` for the
/// implementation that consults
/// ``SimulationLP::find_ampl_variable_for_element`` and sets
/// ``ResolveColResult::element_known`` accordingly.

#include <doctest/doctest.h>
#include <gtopt/planning_options.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

namespace
{

/// Two-block, two-bus fixture with a per-block ``pmax`` schedule that
/// drops to 0 in block 1.  Mirrors the PLEXOS startup-staged
/// generator pattern: the LP column for ``g1.generation`` is created
/// in block 2 (where ``pmax > 0``) but NOT in block 1.
struct PerBlockPmaxFixture
{
  PlanningOptions opts;
  Simulation simulation;
  System system;

  explicit PerBlockPmaxFixture(Array<UserConstraint> ucs)
  {
    opts.model_options.use_single_bus = true;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.model_options.scale_objective = 1.0;

    simulation = Simulation {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1,
                },
                {
                    .uid = Uid {2},
                    .duration = 1,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 2,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {1},
                    .probability_factor = 1.0,
                },
            },
    };

    // ``g1`` has pmax = [0, 100] across the two blocks; GeneratorLP
    // skips the block-1 column entirely (continue at the
    // ``block_pmax == 0 && block_pmin == 0`` guard in
    // ``generator_lp.cpp::add_to_lp``) but emits a normal column for
    // block 2.  This is exactly the pattern that triggered the LNG
    // strict-mode crash on CEN PCP weekly.
    const std::vector<double> pmax_profile = {0.0, 100.0};

    system = System {
        .name = "per_block_pmax_resolver_test",
        .bus_array =
            {
                {
                    .uid = Uid {1},
                    .name = "b1",
                },
            },
        .demand_array =
            {
                {
                    .uid = Uid {1},
                    .name = "d1",
                    .bus = Uid {1},
                    .lmax =
                        std::vector<std::vector<double>> {
                            {
                                0.0,
                                50.0,
                            },
                        },
                },
            },
        .generator_array =
            {
                {
                    .uid = Uid {1},
                    .name = "g1",
                    .bus = Uid {1},
                    .pmax =
                        std::vector<std::vector<double>> {
                            {
                                0.0,
                                100.0,
                            },
                        },
                    .gcost = 10.0,
                },
            },
        .user_constraint_array = std::move(ucs),
    };
  }

  [[nodiscard]] static auto build_matrix_opts() -> LpMatrixOptions
  {
    return LpMatrixOptions {
        .col_with_names = true,
        .row_with_names = true,
    };
  }
};

}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE(  // NOLINT
    "UC resolver — generator.generation on pmax=0 block contributes 0")
{
  // ``g1`` has pmax=0 in block 1 → no LP column → the UC term
  // ``g1.generation`` in block 1 silently contributes 0.  In block 2
  // (pmax=100) the term resolves normally.  The UC must NOT throw
  // strict-mode "missing or inactive" even though block 1 has no
  // column for the referenced generator.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_gen_per_block",
          .expression = "generator('g1').generation <= 100",
      },
  };
  const PerBlockPmaxFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  // The fix is verified by the build-then-resolve succeeding without
  // an exception.  Before the fix, this would throw a
  // ``std::runtime_error`` from
  // ``user_constraint_lp.cpp::build_row_from_terms``'s strict-mode
  // branch on block 1.
  REQUIRE_NOTHROW(
      SystemLP(fix.system, sim_lp, PerBlockPmaxFixture::build_matrix_opts()));
}

TEST_CASE(  // NOLINT
    "UC resolver — typo on per-block fixture still throws (strict-mode "
    "safety guard)")
{
  // A typo on a real element (``g1.generattion`` with a stray ``t``)
  // is NOT registered anywhere in the AMPL variable registry, so
  // ``find_ampl_variable_for_element`` returns false and the
  // strict-mode resolver throws.  Pins the safety guard: only the
  // specific "registered somewhere, missing here" case is tolerated;
  // genuine typos still surface.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_typo",
          .expression = "generator('g1').generattion <= 100",
      },
  };
  const PerBlockPmaxFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, PerBlockPmaxFixture::build_matrix_opts()),
      std::runtime_error);
}

TEST_CASE(  // NOLINT
    "UC resolver — unknown generator name still throws (strict-mode safety "
    "guard)")
{
  // Reference to a generator that doesn't exist: the element-name
  // lookup fails (``lookup_ampl_element_uid`` returns nullopt),
  // ``find_ampl_variable_for_element`` short-circuits to false, and
  // the strict-mode resolver throws.  Pins the safety guard for
  // unknown elements (vs the typo-on-known-element case above).
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_unknown_gen",
          .expression = "generator('g_does_not_exist').generation <= 100",
      },
  };
  const PerBlockPmaxFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, PerBlockPmaxFixture::build_matrix_opts()),
      std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// LP-attr-dormant leniency: generator with pmax=0 on EVERY block.
//
// Distinct from the per-block-dormant case above: GeneratorLP never
// creates any ``generation`` column for the all-zero gen at all, so
// ``add_ampl_variable`` is skipped (it gates on ``!gcols.empty()``)
// and the (class, element_uid, "generation") triple is registered
// NOWHERE for this UID.  The narrower ``find_ampl_variable_for_element``
// check would therefore return false and the strict resolver would
// throw.  The broader ``find_ampl_class_attribute`` check used in the
// extended leniency returns true because ANOTHER generator in the
// system DOES register ``generation`` (the class-level attribute is
// known), so the term silently contributes 0 to the LHS — matching
// PLEXOS's "coefficient × 0 = 0" treatment.
//
// This is the PANGUE_U1 / CEN PCP weekly bundle case: a generator
// with ``pmax = 0`` and no ``pmax_profile`` whose name appears in a
// reservoir / fuel-cap user constraint LHS.
// ─────────────────────────────────────────────────────────────────────────────

/// Two-generator fixture for the LP-attr-dormant test.  ``g_off`` has
/// ``pmax = 0`` on every block (no column ever materialised); ``g_on``
/// has ``pmax > 0`` and DOES register ``generation`` — proving the
/// class-level attribute is valid.
struct AllZeroPmaxFixture
{
  PlanningOptions opts;
  Simulation simulation;
  System system;

  explicit AllZeroPmaxFixture(Array<UserConstraint> ucs)
  {
    opts.model_options.use_single_bus = true;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.model_options.scale_objective = 1.0;

    simulation = Simulation {
        .block_array =
            {
                {
                    .uid = Uid {1},
                    .duration = 1,
                },
                {
                    .uid = Uid {2},
                    .duration = 1,
                },
            },
        .stage_array =
            {
                {
                    .uid = Uid {1},
                    .first_block = 0,
                    .count_block = 2,
                },
            },
        .scenario_array =
            {
                {
                    .uid = Uid {1},
                    .probability_factor = 1.0,
                },
            },
    };

    system = System {
        .name = "all_zero_pmax_resolver_test",
        .bus_array =
            {
                {
                    .uid = Uid {1},
                    .name = "b1",
                },
            },
        .demand_array =
            {
                {
                    .uid = Uid {1},
                    .name = "d1",
                    .bus = Uid {1},
                    .lmax =
                        std::vector<std::vector<double>> {
                            {
                                10.0,
                                10.0,
                            },
                        },
                },
            },
        .generator_array =
            {
                // g_off: pmax = 0 on every block → no LP column,
                // ``generation`` attribute NOT registered for THIS uid.
                {
                    .uid = Uid {1},
                    .name = "g_off",
                    .bus = Uid {1},
                    .pmax = 0.0,
                    .gcost = 10.0,
                },
                // g_on: pmax = 100 → column exists in every block,
                // ``generation`` attribute IS registered for THIS uid.
                {
                    .uid = Uid {2},
                    .name = "g_on",
                    .bus = Uid {1},
                    .pmax = 100.0,
                    .gcost = 20.0,
                },
            },
        .user_constraint_array = std::move(ucs),
    };
  }

  [[nodiscard]] static auto build_matrix_opts() -> LpMatrixOptions
  {
    return LpMatrixOptions {
        .col_with_names = true,
        .row_with_names = true,
    };
  }
};

TEST_CASE(  // NOLINT
    "UC resolver — generator.generation on all-zero-pmax gen contributes 0 "
    "(LP-attr-dormant)")
{
  // ``g_off`` has ``pmax = 0`` on every block; GeneratorLP never
  // creates a ``generation`` column for it (the
  // ``block_pmax == 0 && block_pmin == 0`` early-out fires on each
  // block) and ``add_ampl_variable`` is skipped because the column
  // map is empty.  The (class, g_off, generation) triple is therefore
  // registered NOWHERE.  But (class, generation) IS registered (via
  // g_on), so the LP-attr-dormant leniency kicks in and the UC term
  // ``g_off.generation`` silently contributes 0 on every block.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_all_zero_gen",
          .expression =
              "generator('g_off').generation + generator('g_on').generation "
              "<= 200",
      },
  };
  const AllZeroPmaxFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  // Before the LP-attr-dormant fix, this would throw a
  // ``std::runtime_error`` from
  // ``user_constraint_lp.cpp::build_row_from_terms`` complaining
  // about an unknown ``generation`` attribute on the ``g_off``
  // generator (because no LP column was ever created).
  REQUIRE_NOTHROW(
      SystemLP(fix.system, sim_lp, AllZeroPmaxFixture::build_matrix_opts()));
}

TEST_CASE(  // NOLINT
    "UC resolver — bogus attribute on all-zero gen still throws "
    "(LP-attr-dormant safety guard)")
{
  // A bogus attribute (``g_off.bogus``) is NOT registered for the
  // (class, attribute) pair on ANY element of the class.  The
  // LP-attr-dormant check ``find_ampl_class_attribute("generator",
  // "bogus")`` therefore returns false, and the strict resolver
  // throws.  Pins the safety guard: the LP-attr-dormant leniency
  // only forgives the case where the attribute IS a real LP variable
  // on the class (just not materialised for THIS element).
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_bogus_attr",
          .expression = "generator('g_off').bogus <= 100",
      },
  };
  const AllZeroPmaxFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, AllZeroPmaxFixture::build_matrix_opts()),
      std::runtime_error);
}

TEST_CASE(  // NOLINT
    "UC resolver — bogus attribute on active gen still throws "
    "(LP-attr-dormant safety guard, second variant)")
{
  // Same guard, exercised on a generator that DOES have a generation
  // column.  ``g_on.bogus`` references an attribute that is not
  // registered anywhere on the ``generator`` class → strict throw.
  // Confirms the new leniency does NOT regress the typo-on-known
  // case for an element that has materialised LP columns.
  Array<UserConstraint> ucs = {
      {
          .uid = Uid {1},
          .name = "uc_bogus_attr_active",
          .expression = "generator('g_on').bogus <= 100",
      },
  };
  const AllZeroPmaxFixture fix(std::move(ucs));

  const PlanningOptionsLP options(fix.opts);
  SimulationLP sim_lp(fix.simulation, options);

  CHECK_THROWS_AS(
      SystemLP(fix.system, sim_lp, AllZeroPmaxFixture::build_matrix_opts()),
      std::runtime_error);
}

// NOLINTEND(bugprone-unchecked-optional-access)
