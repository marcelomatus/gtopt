// SPDX-License-Identifier: BSD-3-Clause
#include <limits>

#include <doctest/doctest.h>
#include <gtopt/phase.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Phase construction and default values")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Phase phase;

  CHECK(phase.uid == Uid {unknown_uid});
  CHECK_FALSE(phase.name.has_value());
  CHECK_FALSE(phase.active.has_value());
  CHECK(phase.first_stage == 0);
  CHECK(phase.count_stage == std::dynamic_extent);
  CHECK(phase.apertures.empty());
  CHECK(phase.class_name == "phase");
}

TEST_CASE("Phase is_active default behaviour")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  SUBCASE("default is active when unset")
  {
    const Phase phase;
    CHECK(phase.is_active() == true);
  }

  SUBCASE("explicitly active")
  {
    Phase phase;
    phase.active = true;
    CHECK(phase.is_active() == true);
  }

  SUBCASE("explicitly inactive")
  {
    Phase phase;
    phase.active = false;
    CHECK(phase.is_active() == false);
  }
}

TEST_CASE("Phase attribute assignment")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Phase phase;

  phase.uid = 1;
  phase.name = "construction";
  phase.active = true;
  phase.first_stage = 0;
  phase.count_stage = 5;

  CHECK(phase.uid == 1);
  REQUIRE(phase.name.has_value());
  CHECK(phase.name.value() == "construction");
  CHECK(phase.first_stage == 0);
  CHECK(phase.count_stage == 5);
}

TEST_CASE("Phase designated initializer construction")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Phase phase {
      .uid = Uid {2},
      .name = "operational",
      .active = {},
      .first_stage = 5,
      .count_stage = 20,
  };

  CHECK(phase.uid == Uid {2});
  REQUIRE(phase.name.has_value());
  CHECK(phase.name.value() == "operational");
  CHECK(phase.first_stage == 5);
  CHECK(phase.count_stage == 20);
}

TEST_CASE("Phase with apertures")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Phase phase;
  phase.uid = 3;
  phase.apertures = {Uid {10}, Uid {20}, Uid {30}};

  CHECK(phase.apertures.size() == 3);
  CHECK(phase.apertures[0] == Uid {10});
  CHECK(phase.apertures[1] == Uid {20});
  CHECK(phase.apertures[2] == Uid {30});
}

TEST_CASE("Phase with dynamic_extent covers all stages")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Phase phase {
      .uid = Uid {1},
      .name = {},
      .active = {},
      .first_stage = 3,
      .count_stage = std::dynamic_extent,
  };

  CHECK(phase.first_stage == 3);
  CHECK(phase.count_stage == std::dynamic_extent);
}

TEST_CASE("PhaseUid and PhaseIndex strong types")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const PhaseUid puid = make_uid<Phase>(5);
  const PhaseIndex pidx {2};

  CHECK(puid == make_uid<Phase>(5));
  CHECK(pidx == PhaseIndex {2});
}

TEST_CASE("Phase array construction")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  const Array<Phase> phases {
      {
          .uid = Uid {1},
          .name = {},
          .active = {},
          .first_stage = 0,
          .count_stage = 5,
      },
      {
          .uid = Uid {2},
          .name = {},
          .active = {},
          .first_stage = 5,
          .count_stage = 10,
      },
  };

  CHECK(phases.size() == 2);
  CHECK(phases[0].first_stage == 0);
  CHECK(phases[0].count_stage == 5);
  CHECK(phases[1].first_stage == 5);
  CHECK(phases[1].count_stage == 10);
}
