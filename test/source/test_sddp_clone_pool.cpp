/**
 * @file      test_sddp_clone_pool.hpp
 * @brief     Unit tests for SDDPClonePool
 * @date      2026-03-22
 * @copyright BSD-3-Clause
 */

#include <doctest/doctest.h>
#include <gtopt/sddp_clone_pool.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ---------------------------------------------------------------------------
// SDDPClonePool basic construction
// ---------------------------------------------------------------------------

TEST_CASE("SDDPClonePool default constructed is empty")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const SDDPClonePool pool;
  CHECK_FALSE(pool.is_allocated());
}

TEST_CASE("SDDPClonePool allocate marks pool as allocated")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SDDPClonePool pool;
  pool.allocate(2, 3);
  CHECK(pool.is_allocated());
}

TEST_CASE("SDDPClonePool get_ptr returns nullptr when not allocated")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SDDPClonePool pool;

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);
  auto* ptr = pool.get_ptr(first_scene_index(), first_phase_index(), plp, 0);
  CHECK(ptr == nullptr);
}

TEST_CASE("SDDPClonePool get_or_create returns valid clone")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  const auto num_phases = static_cast<Index>(plp.simulation().phases().size());

  SDDPClonePool pool;
  pool.allocate(num_scenes, num_phases);

  // First access creates the clone
  auto& li =
      pool.get_or_create(first_scene_index(),
                         first_phase_index(),
                         plp,
                         plp.system(first_scene_index(), first_phase_index())
                             .linear_interface()
                             .get_numrows());

  // The clone should have the same structure as the original
  const auto& orig =
      plp.system(first_scene_index(), first_phase_index()).linear_interface();
  CHECK(li.get_numcols() == orig.get_numcols());
  CHECK(li.get_numrows() == orig.get_numrows());
}

TEST_CASE("SDDPClonePool get_or_create reuses clone on second call")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  const auto num_phases = static_cast<Index>(plp.simulation().phases().size());

  SDDPClonePool pool;
  pool.allocate(num_scenes, num_phases);

  const auto base_nrows = plp.system(first_scene_index(), first_phase_index())
                              .linear_interface()
                              .get_numrows();

  auto& li1 = pool.get_or_create(
      first_scene_index(), first_phase_index(), plp, base_nrows);
  auto* addr1 = &li1;

  // Second access resets and returns the same clone
  auto& li2 = pool.get_or_create(
      first_scene_index(), first_phase_index(), plp, base_nrows);
  auto* addr2 = &li2;

  CHECK(addr1 == addr2);
}

TEST_CASE(
    "SDDPClonePool different phases produce independent clones")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  const auto num_phases = static_cast<Index>(plp.simulation().phases().size());
  REQUIRE(num_phases >= 2);

  SDDPClonePool pool;
  pool.allocate(num_scenes, num_phases);

  auto& li0 =
      pool.get_or_create(first_scene_index(),
                         first_phase_index(),
                         plp,
                         plp.system(first_scene_index(), first_phase_index())
                             .linear_interface()
                             .get_numrows());
  auto& li1 = pool.get_or_create(first_scene_index(),
                                 PhaseIndex {1},
                                 plp,
                                 plp.system(first_scene_index(), PhaseIndex {1})
                                     .linear_interface()
                                     .get_numrows());

  CHECK(&li0 != &li1);
}

TEST_CASE("SDDPClonePool get_ptr returns non-null when allocated")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto planning = make_3phase_hydro_planning();
  PlanningLP plp(planning);

  const auto num_scenes = static_cast<Index>(plp.simulation().scenes().size());
  const auto num_phases = static_cast<Index>(plp.simulation().phases().size());

  SDDPClonePool pool;
  pool.allocate(num_scenes, num_phases);

  auto* ptr = pool.get_ptr(first_scene_index(),
                           first_phase_index(),
                           plp,
                           plp.system(first_scene_index(), first_phase_index())
                               .linear_interface()
                               .get_numrows());
  CHECK(ptr != nullptr);
}
