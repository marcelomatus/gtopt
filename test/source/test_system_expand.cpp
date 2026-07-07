// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_system_expand.cpp
 * @brief     Unit tests for System reservoir constraint expansion and merge
 * @date      2026-05-12
 *
 * Tests:
 *   1. expand_reservoir_constraints with empty reservoir_array → no-op
 *   2. expand_reservoir_constraints: seepage with explicit UID/name → preserved
 *   3. expand_reservoir_constraints: seepage with unknown_uid → auto-assigned
 *   4. expand_reservoir_constraints: seepage with empty name → auto-named
 *   5. expand_reservoir_constraints: duplicate UID → reassigned
 *   6. expand_reservoir_constraints: discharge_limit expansion
 *   7. expand_reservoir_constraints: production_factor expansion
 *   8. expand_reservoir_constraints: all three constraint types together
 *   9. expand_reservoir_constraints: idempotent on re-expansion
 *  10. System::merge: non-empty name/version inherits from rhs
 *  11. System::merge: element arrays appended
 *  12. System::merge: user_constraint_file carried over
 *  13. System::merge: user_constraint_files appended
 *  14. System::merge: empty rhs is no-op
 */

#include <string>

#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/reservoir.hpp>
#include <gtopt/reservoir_discharge_limit.hpp>
#include <gtopt/reservoir_production_factor.hpp>
#include <gtopt/reservoir_seepage.hpp>
#include <gtopt/system.hpp>
#include <gtopt/user_constraint.hpp>

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

// ─── expand_reservoir_constraints tests ──────────────────────────────────────

TEST_CASE("expand_reservoir_constraints — empty reservoir_array is no-op")
{
  System sys;
  sys.expand_reservoir_constraints();

  CHECK(sys.reservoir_seepage_array.empty());
  CHECK(sys.reservoir_discharge_limit_array.empty());
  CHECK(sys.reservoir_production_factor_array.empty());
}

TEST_CASE(
    "expand_reservoir_constraints — seepage with explicit UID and name "
    "preserved")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {10},
          .name = "rsv1",
          .seepage =
              {
                  {
                      .uid = Uid {100},
                      .name = "my_seepage",
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  REQUIRE(sys.reservoir_seepage_array.size() == 1);
  CHECK(sys.reservoir_seepage_array[0].uid == Uid {100});
  CHECK(sys.reservoir_seepage_array[0].name == "my_seepage");
  // reservoir field set to parent
  CHECK(sys.reservoir_seepage_array[0].reservoir == SingleId {Uid {10}});
  // Inline array cleared
  CHECK(sys.reservoir_array[0].seepage.empty());
}

TEST_CASE(
    "expand_reservoir_constraints — seepage with unknown_uid gets "
    "auto-assigned")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  REQUIRE(sys.reservoir_seepage_array.size() == 1);
  // unknown_uid should be replaced with next available
  CHECK(sys.reservoir_seepage_array[0].uid != unknown_uid);
  CHECK(sys.reservoir_seepage_array[0].uid == Uid {1});
}

TEST_CASE(
    "expand_reservoir_constraints — seepage with empty name gets "
    "auto-named")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {5},
          .name = "laja",
          .seepage =
              {
                  {
                      .uid = Uid {10},
                      .name = {},
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  REQUIRE(sys.reservoir_seepage_array.size() == 1);
  CHECK(sys.reservoir_seepage_array[0].uid == Uid {10});
  // Auto-named: rsv_name + "_seepage_" + uid
  CHECK(sys.reservoir_seepage_array[0].name == "laja_seepage_10");
}

TEST_CASE("expand_reservoir_constraints — duplicate UID gets reassigned")
{
  System sys;
  // Pre-populate seepage array with UID 2
  sys.reservoir_seepage_array = {
      {
          .uid = Uid {2},
          .name = "existing",
      },
  };
  sys.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .seepage =
              {
                  {
                      .uid = Uid {2},
                      .name = "collision",
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  REQUIRE(sys.reservoir_seepage_array.size() == 2);
  // The duplicate UID 2 in the inline seepage should be reassigned
  CHECK(sys.reservoir_seepage_array[1].uid != Uid {2});
  CHECK(sys.reservoir_seepage_array[1].name == "collision");
}

TEST_CASE("expand_reservoir_constraints — discharge_limit expansion")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .discharge_limit =
              {
                  {
                      .uid = Uid {50},
                      .name = "dlim1",
                  },
                  {
                      .uid = unknown_uid,
                      .name = {},
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  REQUIRE(sys.reservoir_discharge_limit_array.size() == 2);
  CHECK(sys.reservoir_discharge_limit_array[0].uid == Uid {50});
  CHECK(sys.reservoir_discharge_limit_array[0].name == "dlim1");
  CHECK(sys.reservoir_discharge_limit_array[0].reservoir == SingleId {Uid {1}});

  // Second element had unknown_uid → auto-assigned, empty name → auto-named
  CHECK(sys.reservoir_discharge_limit_array[1].uid != unknown_uid);
  CHECK(sys.reservoir_discharge_limit_array[1].name
        == "rsv1_dlim_"
            + std::to_string(sys.reservoir_discharge_limit_array[1].uid));

  // Inline arrays cleared
  CHECK(sys.reservoir_array[0].discharge_limit.empty());
}

TEST_CASE("expand_reservoir_constraints — production_factor expansion")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {3},
          .name = "big_reservoir",
          .production_factor =
              {
                  {
                      .uid = Uid {70},
                      .name = "pfac_main",
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  REQUIRE(sys.reservoir_production_factor_array.size() == 1);
  CHECK(sys.reservoir_production_factor_array[0].uid == Uid {70});
  CHECK(sys.reservoir_production_factor_array[0].name == "pfac_main");
  CHECK(sys.reservoir_production_factor_array[0].reservoir
        == SingleId {Uid {3}});

  CHECK(sys.reservoir_array[0].production_factor.empty());
}

TEST_CASE("expand_reservoir_constraints — all three types together")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .seepage =
              {
                  {
                      .uid = Uid {10},
                      .name = "s1",
                  },
              },
          .discharge_limit =
              {
                  {
                      .uid = Uid {20},
                      .name = "dl1",
                  },
              },
          .production_factor =
              {
                  {
                      .uid = Uid {30},
                      .name = "pf1",
                  },
              },
      },
      {
          .uid = Uid {2},
          .name = "rsv2",
          .seepage =
              {
                  {
                      .uid = unknown_uid,
                      .name = {},
                  },
              },
          .discharge_limit =
              {
                  {
                      .uid = unknown_uid,
                      .name = "dl2",
                  },
              },
          .production_factor =
              {
                  {
                      .uid = Uid {40},
                      .name = {},
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();

  // Seepage: 2 entries
  CHECK(sys.reservoir_seepage_array.size() == 2);
  // Discharge limit: 2 entries
  CHECK(sys.reservoir_discharge_limit_array.size() == 2);
  // Production factor: 2 entries
  CHECK(sys.reservoir_production_factor_array.size() == 2);

  // Verify all inline arrays cleared
  CHECK(sys.reservoir_array[0].seepage.empty());
  CHECK(sys.reservoir_array[0].discharge_limit.empty());
  CHECK(sys.reservoir_array[0].production_factor.empty());
  CHECK(sys.reservoir_array[1].seepage.empty());
  CHECK(sys.reservoir_array[1].discharge_limit.empty());
  CHECK(sys.reservoir_array[1].production_factor.empty());

  // Verify reservoir refs for rsv2
  CHECK(sys.reservoir_seepage_array[1].reservoir == SingleId {Uid {2}});
  CHECK(sys.reservoir_discharge_limit_array[1].name == "dl2");
  CHECK(sys.reservoir_discharge_limit_array[1].reservoir == SingleId {Uid {2}});
  // rsv2 pfac had empty name → auto-named
  CHECK(sys.reservoir_production_factor_array[1].name
        == "rsv2_pfac_"
            + std::to_string(sys.reservoir_production_factor_array[1].uid));
  CHECK(sys.reservoir_production_factor_array[1].reservoir
        == SingleId {Uid {2}});
}

TEST_CASE("expand_reservoir_constraints — idempotent on re-expansion")
{
  System sys;
  sys.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "rsv1",
          .seepage =
              {
                  {
                      .uid = Uid {10},
                      .name = "s1",
                  },
              },
      },
  };

  sys.expand_reservoir_constraints();
  const auto count_after_first = sys.reservoir_seepage_array.size();

  // Re-expand: should be no-op since inline arrays are empty
  sys.expand_reservoir_constraints();
  CHECK(sys.reservoir_seepage_array.size() == count_after_first);
}

// ─── System::merge tests ─────────────────────────────────────────────────────

TEST_CASE("System::merge — name and version inherited from rhs")
{
  System sys_a;
  sys_a.name = "original";
  sys_a.version = "1.0";

  System sys_b;
  sys_b.name = "merged";
  sys_b.version = "2.0";

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.name == "merged");
  CHECK(sys_a.version == "2.0");
}

TEST_CASE("System::merge — empty name/version on rhs preserves lhs")
{
  System sys_a;
  sys_a.name = "keep_me";
  sys_a.version = "v1";

  System sys_b;
  // name and version left empty

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.name == "keep_me");
  CHECK(sys_a.version == "v1");
}

TEST_CASE("System::merge — element arrays are appended")
{
  System sys_a;
  sys_a.bus_array = {
      {.uid = Uid {1}, .name = "bus_a"},
  };
  sys_a.generator_array = {
      {.uid = Uid {1}, .name = "gen_a"},
  };

  System sys_b;
  sys_b.bus_array = {
      {.uid = Uid {2}, .name = "bus_b"},
  };
  sys_b.demand_array = {
      {.uid = Uid {1}, .name = "dem_b"},
  };

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.bus_array.size() == 2);
  CHECK(sys_a.bus_array[0].name == "bus_a");
  CHECK(sys_a.bus_array[1].name == "bus_b");

  CHECK(sys_a.generator_array.size() == 1);
  CHECK(sys_a.demand_array.size() == 1);
  CHECK(sys_a.demand_array[0].name == "dem_b");
}

TEST_CASE("System::merge — user_constraint_file carried over")
{
  System sys_a;
  System sys_b;
  sys_b.user_constraint_file = Name {"extra_constraints.json"};

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.user_constraint_file.has_value());
  CHECK(sys_a.user_constraint_file.value() == "extra_constraints.json");
}

TEST_CASE("System::merge — user_constraint_files appended")
{
  System sys_a;
  sys_a.user_constraint_files = {Name {"a.json"}};

  System sys_b;
  sys_b.user_constraint_files = {Name {"b.json"}, Name {"c.json"}};

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.user_constraint_files.size() == 3);
  CHECK(sys_a.user_constraint_files[0] == "a.json");
  CHECK(sys_a.user_constraint_files[1] == "b.json");
  CHECK(sys_a.user_constraint_files[2] == "c.json");
}

TEST_CASE("System::merge — empty rhs is no-op")
{
  System sys_a;
  sys_a.name = "original";
  sys_a.bus_array = {
      {.uid = Uid {1}, .name = "bus1"},
  };

  System sys_b;

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.name == "original");
  CHECK(sys_a.bus_array.size() == 1);
  CHECK(sys_a.bus_array[0].name == "bus1");
}

TEST_CASE("System::merge — hydro arrays are merged")
{
  System sys_a;
  sys_a.reservoir_array = {
      {.uid = Uid {1}, .name = "r_a"},
  };
  sys_a.turbine_array = {
      {.uid = Uid {1}, .name = "t_a"},
  };

  System sys_b;
  sys_b.reservoir_array = {
      {.uid = Uid {2}, .name = "r_b"},
  };
  sys_b.junction_array = {
      {.uid = Uid {1}, .name = "j_b"},
  };
  sys_b.waterway_array = {
      {.uid = Uid {1}, .name = "w_b"},
  };
  sys_b.flow_array = {
      {.uid = Uid {1}, .name = "f_b"},
  };
  sys_b.pump_array = {
      {.uid = Uid {1}, .name = "p_b"},
  };
  sys_b.flow_right_array = {
      {.uid = Uid {1}, .name = "fr_b"},
  };
  sys_b.volume_right_array = {
      {.uid = Uid {1}, .name = "vr_b"},
  };

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.reservoir_array.size() == 2);
  CHECK(sys_a.turbine_array.size() == 1);
  CHECK(sys_a.junction_array.size() == 1);
  CHECK(sys_a.waterway_array.size() == 1);
  CHECK(sys_a.flow_array.size() == 1);
  CHECK(sys_a.pump_array.size() == 1);
  CHECK(sys_a.flow_right_array.size() == 1);
  CHECK(sys_a.volume_right_array.size() == 1);
}

TEST_CASE("System::merge — profiles and storage arrays are merged")
{
  System sys_a;
  sys_a.generator_profile_array = {
      {.uid = Uid {1}, .name = "gp_a"},
  };
  sys_a.battery_array = {
      {.uid = Uid {1}, .name = "bat_a"},
  };
  sys_a.converter_array = {
      {.uid = Uid {1}, .name = "conv_a"},
  };
  sys_a.lng_terminal_array = {
      {.uid = Uid {1}, .name = "lng_a"},
  };

  System sys_b;
  sys_b.demand_profile_array = {
      {.uid = Uid {1}, .name = "dp_b"},
  };
  sys_b.commitment_array = {
      {.uid = Uid {1}, .name = "cmt_b"},
  };
  sys_b.simple_commitment_array = {
      {.uid = Uid {1}, .name = "sc_b"},
  };
  sys_b.reserve_zone_array = {
      {.uid = Uid {1}, .name = "rz_b"},
  };
  sys_b.reserve_provision_array = {
      {.uid = Uid {1}, .name = "rp_b"},
  };
  sys_b.inertia_zone_array = {
      {.uid = Uid {1}, .name = "iz_b"},
  };
  sys_b.inertia_provision_array = {
      {.uid = Uid {1}, .name = "ip_b"},
  };

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.generator_profile_array.size() == 1);
  CHECK(sys_a.demand_profile_array.size() == 1);
  CHECK(sys_a.battery_array.size() == 1);
  CHECK(sys_a.converter_array.size() == 1);
  CHECK(sys_a.lng_terminal_array.size() == 1);
  CHECK(sys_a.commitment_array.size() == 1);
  CHECK(sys_a.simple_commitment_array.size() == 1);
  CHECK(sys_a.reserve_zone_array.size() == 1);
  CHECK(sys_a.reserve_provision_array.size() == 1);
  CHECK(sys_a.inertia_zone_array.size() == 1);
  CHECK(sys_a.inertia_provision_array.size() == 1);
}

TEST_CASE("System::merge — user parameters and constraints are merged")
{
  System sys_a;
  sys_a.user_constraint_array = {
      {.uid = Uid {1}, .name = "uc_a"},
  };

  System sys_b;
  sys_b.user_constraint_array = {
      {.uid = Uid {2}, .name = "uc_b"},
  };
  sys_b.user_constraint_file = Name {"extra.json"};

  sys_a.merge(std::move(sys_b));

  CHECK(sys_a.user_constraint_array.size() == 2);
  CHECK(sys_a.user_constraint_file.has_value());
  CHECK(sys_a.user_constraint_file.value() == "extra.json");
}

// NOLINTEND(bugprone-unchecked-optional-access)