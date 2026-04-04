#pragma once

#include <doctest/doctest.h>
#include <gtopt/system.hpp>

TEST_CASE("expand_reservoir_constraints moves embedded seepage to system array")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {10},
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {5},
                      .slope = 0.001,
                      .constant = 0.5,
                  },
              },
      },
  };

  CHECK(system.reservoir_seepage_array.empty());
  system.expand_reservoir_constraints();

  // Embedded seepage should have been moved to the system-level array
  REQUIRE(system.reservoir_seepage_array.size() == 1);
  CHECK(system.reservoir_array[0].seepage.empty());

  const auto& seep = system.reservoir_seepage_array[0];
  CHECK(seep.uid != unknown_uid);  // Auto-assigned
  CHECK(seep.name == "rsv1_seepage_1");
  CHECK(std::get<Uid>(seep.reservoir) == Uid {1});
  CHECK(std::get<Uid>(seep.waterway) == Uid {5});
  CHECK(std::get<Real>(seep.slope.value_or(TRealFieldSched {0.0}))
        == doctest::Approx(0.001));
}

TEST_CASE(
    "expand_reservoir_constraints moves embedded discharge_limit to system "
    "array")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {10},
          .discharge_limit =
              {
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {5},
                      .reservoir = Uid {99},  // Will be overwritten
                      .segments =
                          {
                              {.volume = 0.0, .slope = 1e-4, .intercept = 10.0},
                          },
                  },
              },
      },
  };

  CHECK(system.reservoir_discharge_limit_array.empty());
  system.expand_reservoir_constraints();

  REQUIRE(system.reservoir_discharge_limit_array.size() == 1);
  CHECK(system.reservoir_array[0].discharge_limit.empty());

  const auto& ddl = system.reservoir_discharge_limit_array[0];
  CHECK(ddl.uid != unknown_uid);
  CHECK(ddl.name == "rsv1_dlim_1");
  CHECK(std::get<Uid>(ddl.reservoir) == Uid {1});  // Overwritten to rsv uid
  CHECK(std::get<Uid>(ddl.waterway) == Uid {5});
  REQUIRE(ddl.segments.size() == 1);
  CHECK(ddl.segments[0].intercept == doctest::Approx(10.0));
}

TEST_CASE(
    "expand_reservoir_constraints moves embedded production_factor to system "
    "array")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {10},
          .production_factor =
              {
                  {
                      .uid = unknown_uid,
                      .turbine = Uid {7},
                      .mean_production_factor = 1.5,
                      .segments =
                          {
                              {.volume = 0.0, .slope = 0.001, .constant = 1.0},
                          },
                  },
              },
      },
  };

  CHECK(system.reservoir_production_factor_array.empty());
  system.expand_reservoir_constraints();

  REQUIRE(system.reservoir_production_factor_array.size() == 1);
  CHECK(system.reservoir_array[0].production_factor.empty());

  const auto& pfac = system.reservoir_production_factor_array[0];
  CHECK(pfac.uid != unknown_uid);
  CHECK(pfac.name == "rsv1_pfac_1");
  CHECK(std::get<Uid>(pfac.reservoir) == Uid {1});
  CHECK(std::get<Uid>(pfac.turbine) == Uid {7});
  CHECK(pfac.mean_production_factor == doctest::Approx(1.5));
}

TEST_CASE("expand_reservoir_constraints preserves existing uid and name")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {10},
          .seepage =
              {
                  {
                      .uid = 42,
                      .name = "custom_seep",
                      .waterway = Uid {5},
                      .slope = 0.001,
                  },
              },
      },
  };

  system.expand_reservoir_constraints();

  REQUIRE(system.reservoir_seepage_array.size() == 1);
  const auto& seep = system.reservoir_seepage_array[0];
  CHECK(seep.uid == 42);  // Preserved
  CHECK(seep.name == "custom_seep");  // Preserved
}

TEST_CASE("expand_reservoir_constraints handles multiple reservoirs")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {10},
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {5},
                      .slope = 0.001,
                  },
              },
      },
      {
          .uid = Uid {2},
          .name = "rsv2",
          .junction = Uid {20},
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {6},
                      .slope = 0.002,
                  },
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {7},
                      .slope = 0.003,
                  },
              },
      },
  };

  system.expand_reservoir_constraints();

  REQUIRE(system.reservoir_seepage_array.size() == 3);
  CHECK(system.reservoir_array[0].seepage.empty());
  CHECK(system.reservoir_array[1].seepage.empty());

  // Each gets a unique uid
  CHECK(system.reservoir_seepage_array[0].uid
        != system.reservoir_seepage_array[1].uid);
  CHECK(system.reservoir_seepage_array[1].uid
        != system.reservoir_seepage_array[2].uid);

  // Reservoir IDs are correctly assigned
  CHECK(std::get<Uid>(system.reservoir_seepage_array[0].reservoir) == Uid {1});
  CHECK(std::get<Uid>(system.reservoir_seepage_array[1].reservoir) == Uid {2});
  CHECK(std::get<Uid>(system.reservoir_seepage_array[2].reservoir) == Uid {2});
}

TEST_CASE(
    "expand_reservoir_constraints appends to existing system-level arrays")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  // Pre-existing system-level seepage
  system.reservoir_seepage_array = {
      {
          .uid = 100,
          .name = "existing_seep",
          .waterway = Uid {1},
          .reservoir = Uid {1},
      },
  };
  system.reservoir_array = {
      {
          .uid = Uid {2},
          .name = "rsv2",
          .junction = Uid {20},
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {5},
                      .slope = 0.001,
                  },
              },
      },
  };

  system.expand_reservoir_constraints();

  // New element appended after existing one
  REQUIRE(system.reservoir_seepage_array.size() == 2);
  CHECK(system.reservoir_seepage_array[0].uid == 100);
  CHECK(system.reservoir_seepage_array[1].uid == 101);  // next_uid after 100
  CHECK(system.reservoir_seepage_array[1].name == "rsv2_seepage_101");
}

TEST_CASE("expand_reservoir_constraints is idempotent")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .junction = Uid {10},
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                      .waterway = Uid {5},
                      .slope = 0.001,
                  },
              },
      },
  };

  system.expand_reservoir_constraints();
  REQUIRE(system.reservoir_seepage_array.size() == 1);

  // Second call should not add more elements (embedded vectors were cleared)
  system.expand_reservoir_constraints();
  CHECK(system.reservoir_seepage_array.size() == 1);
}

TEST_CASE(
    "expand_reservoir_constraints reassigns duplicate uid from inline entry")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  // Pre-existing system-level seepage with uid=1
  system.reservoir_seepage_array = {
      {
          .uid = 1,
          .name = "existing_seep",
          .waterway = Uid {1},
          .reservoir = Uid {1},
      },
  };
  system.reservoir_array = {
      {
          .uid = Uid {2},
          .name = "COLBUN",
          .junction = Uid {20},
          .seepage =
              {
                  {
                      // Inline entry with uid=1 that conflicts with existing
                      .uid = 1,
                      .waterway = Uid {5},
                      .slope = 0.001,
                  },
              },
      },
  };

  // Should NOT throw — duplicate uid is reassigned
  system.expand_reservoir_constraints();

  REQUIRE(system.reservoir_seepage_array.size() == 2);
  // The existing entry keeps uid=1
  CHECK(system.reservoir_seepage_array[0].uid == 1);
  CHECK(system.reservoir_seepage_array[0].name == "existing_seep");
  // The inline entry gets a new uid (next_uid=2) since uid=1 was taken
  CHECK(system.reservoir_seepage_array[1].uid == 2);
  // Name is auto-generated with the new uid
  CHECK(system.reservoir_seepage_array[1].name == "COLBUN_seepage_2");
}

TEST_CASE(
    "expand_reservoir_constraints reassigns duplicate name from inline entry")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  System system;
  // Pre-existing system-level seepage with name="COLBUN_seepage_1"
  system.reservoir_seepage_array = {
      {
          .uid = 1,
          .name = "COLBUN_seepage_1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
      },
  };
  system.reservoir_array = {
      {
          .uid = Uid {2},
          .name = "COLBUN",
          .junction = Uid {20},
          .seepage =
              {
                  {
                      // Inline entry with conflicting name
                      .uid = unknown_uid,
                      .name = "COLBUN_seepage_1",
                      .waterway = Uid {5},
                      .slope = 0.002,
                  },
              },
      },
  };

  // Should NOT throw — duplicate name is reassigned
  system.expand_reservoir_constraints();

  REQUIRE(system.reservoir_seepage_array.size() == 2);
  CHECK(system.reservoir_seepage_array[0].name == "COLBUN_seepage_1");
  // The inline entry gets a new auto-generated name
  CHECK(system.reservoir_seepage_array[1].name != "COLBUN_seepage_1");
}
