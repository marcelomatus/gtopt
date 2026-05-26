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

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
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

// NOLINTEND(bugprone-unchecked-optional-access)
