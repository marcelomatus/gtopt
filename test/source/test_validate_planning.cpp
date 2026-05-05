// SPDX-License-Identifier: BSD-3-Clause
#include <algorithm>
#include <ranges>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/validate_planning.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

using namespace gtopt;  // NOLINT(google-build-using-namespace)

/// Build a minimal valid Planning with one bus, one block, one stage.
[[nodiscard]] Planning make_minimal_planning()
{
  Planning p;
  p.system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  p.simulation.block_array = {
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
  };
  p.simulation.stage_array = {
      {
          .uid = Uid {1},
          .count_block = 1,
      },
  };
  return p;
}

TEST_CASE("validate_planning - minimal valid planning passes")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.errors.empty());
  CHECK(result.warnings.empty());
}

TEST_CASE("validate_planning - empty bus_array is an error")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.system.bus_array.clear();
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE("validate_planning - empty block_array is an error")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.block_array.clear();
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE("validate_planning - empty stage_array is an error")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.stage_array.clear();
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE(
    "validate_planning - all empty arrays reports multiple errors")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  Planning p;
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 3);
}

TEST_CASE("validate_planning - block duration <= 0 is an error")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.block_array[0].duration = 0.0;
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());

  SUBCASE("negative duration")
  {
    auto p2 = make_minimal_planning();
    p2.simulation.block_array[0].duration = -5.0;
    auto r2 = validate_planning(p2);
    CHECK_FALSE(r2.ok());
  }
}

TEST_CASE("validate_planning - stage count_block == 0 is an error")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.stage_array[0].count_block = 0;
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
}

TEST_CASE(
    "validate_planning - generator negative capacity is a warning")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  gen.capacity = RealFieldSched {
      -10.0,
  };
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  // Negative capacity is a warning, not an error
  CHECK(result.ok());
  CHECK(result.warnings.size() >= 1);
}

TEST_CASE(
    "validate_planning - generator with valid capacity has no warnings")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  gen.capacity = RealFieldSched {
      100.0,
  };
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.warnings.empty());
}

TEST_CASE("validate_planning - generator referencing invalid bus")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {999};  // Non-existent bus
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

TEST_CASE("validate_planning - demand referencing invalid bus")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Demand dem;
  dem.uid = Uid {1};
  dem.name = "d1";
  dem.bus = Uid {999};  // Non-existent bus
  p.system.demand_array.push_back(dem);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
}

TEST_CASE(
    "validate_planning - line referencing invalid bus_a and bus_b")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Line line;
  line.uid = Uid {1};
  line.name = "l1";
  line.bus_a = Uid {999};
  line.bus_b = Uid {998};
  p.system.line_array.push_back(line);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  // Both bus_a and bus_b are invalid → 2 errors
  CHECK(result.errors.size() >= 2);
}

TEST_CASE("validate_planning - line with valid bus references")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Line line;
  line.uid = Uid {1};
  line.name = "l1";
  line.bus_a = Uid {1};  // Valid bus
  line.bus_b = Uid {1};  // Same bus, but valid reference
  p.system.line_array.push_back(line);
  auto result = validate_planning(p);
  CHECK(result.ok());
}

TEST_CASE("validate_planning - generator referenced by name (valid)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Name {"b1"};  // Reference bus by name
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK(result.ok());
}

TEST_CASE(
    "validate_planning - generator referenced by name (invalid)")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Name {"nonexistent_bus"};
  p.system.generator_array.push_back(gen);
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
}

TEST_CASE("validate_planning - converter with invalid refs")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();

  // Add a battery, generator, demand for valid refs
  Battery bat;
  bat.uid = Uid {1};
  bat.name = "bat1";
  bat.bus = Uid {1};
  p.system.battery_array.push_back(bat);

  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  p.system.generator_array.push_back(gen);

  Demand dem;
  dem.uid = Uid {1};
  dem.name = "d1";
  dem.bus = Uid {1};
  p.system.demand_array.push_back(dem);

  SUBCASE("all valid refs")
  {
    Converter conv;
    conv.uid = Uid {1};
    conv.name = "conv1";
    conv.battery = Uid {1};
    conv.generator = Uid {1};
    conv.demand = Uid {1};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("invalid battery ref")
  {
    Converter conv;
    conv.uid = Uid {2};
    conv.name = "conv2";
    conv.battery = Uid {999};
    conv.generator = Uid {1};
    conv.demand = Uid {1};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("invalid generator ref")
  {
    Converter conv;
    conv.uid = Uid {3};
    conv.name = "conv3";
    conv.battery = Uid {1};
    conv.generator = Uid {999};
    conv.demand = Uid {1};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("invalid demand ref")
  {
    Converter conv;
    conv.uid = Uid {4};
    conv.name = "conv4";
    conv.battery = Uid {1};
    conv.generator = Uid {1};
    conv.demand = Uid {999};
    p.system.converter_array.push_back(conv);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }
}

TEST_CASE("validate_planning - hydro element refs")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();

  // Add junctions
  Junction j1;
  j1.uid = Uid {1};
  j1.name = "j1";
  p.system.junction_array.push_back(j1);

  Junction j2;
  j2.uid = Uid {2};
  j2.name = "j2";
  p.system.junction_array.push_back(j2);

  SUBCASE("waterway with valid junctions")
  {
    Waterway ww;
    ww.uid = Uid {1};
    ww.name = "ww1";
    ww.junction_a = Uid {1};
    ww.junction_b = Uid {2};
    p.system.waterway_array.push_back(ww);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("waterway with invalid junction_a")
  {
    Waterway ww;
    ww.uid = Uid {1};
    ww.name = "ww1";
    ww.junction_a = Uid {999};
    ww.junction_b = Uid {2};
    p.system.waterway_array.push_back(ww);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("flow with valid junction")
  {
    Flow fl;
    fl.uid = Uid {1};
    fl.name = "fl1";
    fl.junction = Uid {1};
    p.system.flow_array.push_back(fl);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("flow with invalid junction")
  {
    Flow fl;
    fl.uid = Uid {1};
    fl.name = "fl1";
    fl.junction = Uid {999};
    p.system.flow_array.push_back(fl);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("reservoir with valid junction")
  {
    Reservoir res;
    res.uid = Uid {1};
    res.name = "res1";
    res.junction = Uid {1};
    p.system.reservoir_array.push_back(res);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("reservoir with invalid junction")
  {
    Reservoir res;
    res.uid = Uid {1};
    res.name = "res1";
    res.junction = Uid {999};
    p.system.reservoir_array.push_back(res);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }
}

TEST_CASE("validate_planning - turbine refs")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();

  // Add generator and waterway/junction for valid refs
  Generator gen;
  gen.uid = Uid {1};
  gen.name = "g1";
  gen.bus = Uid {1};
  p.system.generator_array.push_back(gen);

  Junction j1;
  j1.uid = Uid {1};
  j1.name = "j1";
  p.system.junction_array.push_back(j1);

  Junction j2;
  j2.uid = Uid {2};
  j2.name = "j2";
  p.system.junction_array.push_back(j2);

  Waterway ww;
  ww.uid = Uid {1};
  ww.name = "ww1";
  ww.junction_a = Uid {1};
  ww.junction_b = Uid {2};
  p.system.waterway_array.push_back(ww);

  SUBCASE("turbine with valid refs")
  {
    Turbine turb;
    turb.uid = Uid {1};
    turb.name = "t1";
    turb.waterway = Uid {1};
    turb.generator = Uid {1};
    p.system.turbine_array.push_back(turb);
    auto result = validate_planning(p);
    CHECK(result.ok());
  }

  SUBCASE("turbine with invalid waterway")
  {
    Turbine turb;
    turb.uid = Uid {1};
    turb.name = "t1";
    turb.waterway = Uid {999};
    turb.generator = Uid {1};
    p.system.turbine_array.push_back(turb);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }

  SUBCASE("turbine with invalid generator")
  {
    Turbine turb;
    turb.uid = Uid {1};
    turb.name = "t1";
    turb.waterway = Uid {1};
    turb.generator = Uid {999};
    p.system.turbine_array.push_back(turb);
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
  }
}

TEST_CASE("ValidationResult - ok() semantics")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  ValidationResult r;
  CHECK(r.ok());

  r.warnings.emplace_back("a warning");
  CHECK(r.ok());  // warnings don't affect ok()

  r.errors.emplace_back("an error");
  CHECK_FALSE(r.ok());
}

// ---------------------------------------------------------------------------
// Scenario probability validation
// ---------------------------------------------------------------------------

TEST_CASE(
    "validate_planning - scenario probabilities summing to 1.0")  // NOLINT
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.scenario_array = {
      {
          .uid = Uid {1},
          .probability_factor = 0.6,
      },
      {
          .uid = Uid {2},
          .probability_factor = 0.4,
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.warnings.empty());
}

TEST_CASE(  // NOLINT
    "validate_planning - scenario probabilities not summing to 1.0 warns")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.scenario_array = {
      {
          .uid = Uid {1},
          .probability_factor = 0.7,
      },
      {
          .uid = Uid {2},
          .probability_factor = 0.5,
      },
  };

  SUBCASE("default (runtime) rescales and warns")
  {
    auto result = validate_planning(p);
    CHECK(result.ok());
    CHECK(result.warnings.size() >= 1);
    // Probabilities should be rescaled to sum 1.0
    const double sum = p.simulation.scenario_array[0].probability_factor.value()
        + p.simulation.scenario_array[1].probability_factor.value();
    CHECK(sum == doctest::Approx(1.0));
  }

  SUBCASE("none mode warns but does not rescale")
  {
    p.simulation.probability_rescale = ProbabilityRescaleMode::none;
    const double orig0 = 0.7;
    const double orig1 = 0.5;
    auto result = validate_planning(p);
    CHECK(result.ok());
    CHECK(result.warnings.size() >= 1);
    // Probabilities should remain unchanged
    CHECK(p.simulation.scenario_array[0].probability_factor.value()
          == doctest::Approx(orig0));
    CHECK(p.simulation.scenario_array[1].probability_factor.value()
          == doctest::Approx(orig1));
  }
}

TEST_CASE(  // NOLINT
    "validate_planning - per-scene probability rescaling with scenes")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.scenario_array = {
      {
          .uid = Uid {1},
          .probability_factor = 0.3,
      },
      {
          .uid = Uid {2},
          .probability_factor = 0.5,
      },
  };
  p.simulation.scene_array = {
      {
          .uid = Uid {1},
          .name = "s1",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 1,
      },
      {
          .uid = Uid {2},
          .name = "s2",
          .active = true,
          .first_scenario = 1,
          .count_scenario = 1,
      },
  };

  auto result = validate_planning(p);
  CHECK(result.ok());
  // Warnings about per-scene probability not summing to 1.0,
  // and about total scene probability not summing to 1.0
  CHECK(result.warnings.size() >= 1);
}

TEST_CASE(  // NOLINT
    "validate_planning - scene scenario range exceeds array is an error")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();
  p.simulation.scenario_array = {
      {
          .uid = Uid {1},
          .probability_factor = 1.0,
      },
  };
  p.simulation.scene_array = {
      {
          .uid = Uid {1},
          .name = "bad",
          .active = true,
          .first_scenario = 0,
          .count_scenario = 5,
      },
  };
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  CHECK(result.errors.size() >= 1);
}

// ── Positivity checks ────────────────────────────────────────────────

TEST_CASE("validate_planning - negative FlowRight fmax is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.junction_array = {{.uid = Uid {1}, .name = "j1"}};
  p.system.flow_right_array = {
      {
          .uid = Uid {1},
          .name = "fr_bad",
          .junction = Uid {1},
          .fmax = Real {-5.0},
      },
  };
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e) { return e.find("fmax") != std::string::npos; });
  CHECK(found);
}

TEST_CASE(
    "validate_planning - negative VolumeRight emax is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vr_bad",
          .emax = Real {-100.0},
      },
  };
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e) { return e.find("emax") != std::string::npos; });
  CHECK(found);
}

TEST_CASE(
    "validate_planning - negative VolumeRight eini is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vr_bad",
          .eini = Real {-1.0},
      },
  };
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e) { return e.find("eini") != std::string::npos; });
  CHECK(found);
}

TEST_CASE("validate_planning - negative Line tmax_ab is an error")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };
  p.system.line_array = {
      {
          .uid = Uid {1},
          .name = "l_bad",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .tmax_ba = Real {100.0},
          .tmax_ab = Real {-50.0},
      },
  };
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e) { return e.find("tmax_ab") != std::string::npos; });
  CHECK(found);
}

TEST_CASE(
    "validate_planning - Waterway fmax may be zero (PLP parity)")  // NOLINT
{
  // PLP plpcnfce.dat allows VertMax = 0 to hard-pin the per-block
  // vertimiento waterway at 0 flow (see leecnfce.f:342-343 +
  // genpdver.f:163).  gtopt's validator used to reject fmax=0 under
  // strict positivity, which forced plp2gtopt to translate 0 → None
  // and silently produce an UNBOUNDED spillway.  Now fmax >= 0 is
  // accepted: fmin=fmax=0 legally pins the flow.
  auto p = make_minimal_planning();
  p.system.junction_array = {
      {.uid = Uid {1}, .name = "jA"},
      {.uid = Uid {2}, .name = "jB", .drain = true},
  };
  p.system.waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_zero",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = Real {0.0},
          .fmax = Real {0.0},  // PLP-style pinned-at-zero waterway
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
}

TEST_CASE("validate_planning - Waterway fmax negative is rejected")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.junction_array = {
      {.uid = Uid {1}, .name = "jA"},
      {.uid = Uid {2}, .name = "jB", .drain = true},
  };
  p.system.waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_neg",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = Real {0.0},
          .fmax = Real {-1.0},  // negative fmax rejected (non_negative)
      },
  };
  auto result = validate_planning(p);
  CHECK_FALSE(result.ok());
  const bool found = std::ranges::any_of(
      result.errors,
      [](const auto& e) { return e.find("fmax") != std::string::npos; });
  CHECK(found);
}

TEST_CASE(
    "validate_planning - positivity accepts valid hydro fields")  // NOLINT
{
  auto p = make_minimal_planning();
  p.system.junction_array = {
      {.uid = Uid {1}, .name = "jA"},
      {.uid = Uid {2}, .name = "jB", .drain = true},
  };
  p.system.waterway_array = {
      {
          .uid = Uid {1},
          .name = "ww_ok",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = Real {0.0},
          .fmax = Real {100.0},
      },
  };
  p.system.volume_right_array = {
      {
          .uid = Uid {1},
          .name = "vr_ok",
          .emax = Real {500.0},
          .eini = Real {0.0},
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
}

// ── Per-segment piecewise feasibility (seepage + DRL) ───────────────────────
//
// `check_piecewise_feasibility` walks every segment of every
// `ReservoirSeepage` and `ReservoirDischargeLimit` element,
// evaluates `f(efin) = constant + slope · efin` at the segment's
// active `[V_low, V_high]` range (clipped to the reservoir's
// `[emin, emax]` envelope), and emits a warning when the resulting
// flow range violates the LP-row's bound.  Tests below pin the
// warn-only contract: legal data → no warnings; infeasible
// segments → exactly one warning with the right element name +
// segment index.

[[nodiscard]] Planning make_minimal_with_reservoir_and_waterway()
{
  auto p = make_minimal_planning();
  p.system.junction_array = {
      {.uid = Uid {1}, .name = "j1"},
      {.uid = Uid {2}, .name = "j2"},
  };
  p.system.reservoir_array = {
      {
          .uid = Uid {1},
          .name = "r1",
          .junction = Uid {1},
          .emin = Real {100.0},
          .emax = Real {1000.0},
          .eini = Real {500.0},
      },
  };
  p.system.waterway_array = {
      {
          .uid = Uid {1},
          .name = "w1",
          .junction_a = Uid {1},
          .junction_b = Uid {2},
          .fmin = Real {0.0},
          .fmax = Real {100.0},
      },
  };
  return p;
}

TEST_CASE(  // NOLINT
    "validate_planning - seepage with feasible segments emits no warning")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // f(efin) over [100, 1000] = constant + slope·efin.
  // segment 0: f(100)=0, f(500)=4 — qfilt range [0, 4] ∈ [0, 100] ✓
  // segment 1: f(500)=4, f(1000)=29 — qfilt range [4, 29] ∈ [0, 100] ✓
  p.system.reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "seep1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 100.0,
                      .slope = 0.01,
                      .constant = -1.0,
                  },
                  {
                      .volume = 500.0,
                      .slope = 0.05,
                      .constant = -21.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.warnings.empty());
}

TEST_CASE(  // NOLINT
    "validate_planning - seepage segment below waterway fmin warns")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // Segment 0: f(100) = -5 + 0.01·100 = -4 < fmin=0 — INFEASIBLE.
  p.system.reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "seep1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 100.0,
                      .slope = 0.01,
                      .constant = -5.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  // Warn-only — `result.ok()` still true (no errors).
  CHECK(result.ok());
  REQUIRE(result.warnings.size() >= 1);
  // Confirm the warning message names the right element + bound.
  const auto found =
      std::ranges::any_of(result.warnings,
                          [](const auto& w)
                          {
                            return w.find("seep1") != std::string::npos
                                && w.find("fmin") != std::string::npos
                                && w.find("segment 0") != std::string::npos;
                          });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "validate_planning - seepage segment above waterway fmax warns")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // Segment 0: f(1000) = -50 + 0.2·1000 = 150 > fmax=100 — exceeds upper.
  p.system.reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "seep1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 100.0,
                      .slope = 0.20,
                      .constant = -50.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
  REQUIRE(result.warnings.size() >= 1);
  const auto found =
      std::ranges::any_of(result.warnings,
                          [](const auto& w)
                          {
                            return w.find("seep1") != std::string::npos
                                && w.find("fmax") != std::string::npos;
                          });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "validate_planning - discharge_limit feasible segments emit no warning")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // f(efin) over [100, 1000] = intercept + slope·efin.
  // segment 0: f(100)=0, f(1000)=900 — bound range [0, 900] all >= 0 ✓.
  p.system.reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 100.0,
                      .slope = 1.0,
                      .intercept = -100.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
  CHECK(result.warnings.empty());
}

TEST_CASE(  // NOLINT
    "validate_planning - discharge_limit segment going negative warns")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // Segment 0: f(100) = -3 + 0.02·100 = -1 < 0 — INFEASIBLE.
  p.system.reservoir_discharge_limit_array = {
      {
          .uid = Uid {1},
          .name = "ddl1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 100.0,
                      .slope = 0.02,
                      .intercept = -3.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
  REQUIRE(result.warnings.size() >= 1);
  const auto found =
      std::ranges::any_of(result.warnings,
                          [](const auto& w)
                          {
                            return w.find("ddl1") != std::string::npos
                                && w.find("negative") != std::string::npos;
                          });
  CHECK(found);
}

// ── Phase aperture UID validation ───────────────────────────────────────────
//
// `check_aperture_references` walks every `Phase::apertures` list and
// ensures each UID resolves to an `Aperture` entry in
// `simulation.aperture_array`.  Pre-fix, dangling UIDs silently
// dropped at runtime — `SDDP Aperture [...]: source_scenario X not
// found and no aperture cache, skipping` was emitted per broken
// reference, easily misread as expected behaviour.  Validation now
// promotes this to a hard error at parse time.

TEST_CASE(  // NOLINT
    "validate_planning - phase aperture UID references aperture_array")
{
  using namespace gtopt;  // NOLINT(google-build-using-namespace)

  auto p = make_minimal_planning();

  // Phases must exist to attach `apertures` to.  The minimal
  // fixture has none, so add a single phase referencing the
  // single stage.
  p.simulation.phase_array = {
      {
          .uid = Uid {1},
          .first_stage = Size {0},
          .count_stage = Size {1},
      },
  };

  // 2 apertures defined at simulation level.
  p.simulation.aperture_array = {
      {
          .uid = Uid {51},
          .source_scenario = Uid {51},
          .probability_factor = 0.5,
      },
      {
          .uid = Uid {52},
          .source_scenario = Uid {52},
          .probability_factor = 0.5,
      },
  };

  // Add scenarios so referential checks pass.
  p.simulation.scenario_array = {
      {
          .uid = Uid {51},
      },
      {
          .uid = Uid {52},
      },
  };

  SUBCASE("phase apertures all resolve → no error")
  {
    p.simulation.phase_array.front().apertures = {Uid {51}, Uid {52}};
    auto result = validate_planning(p);
    const auto aperture_errs = std::ranges::count_if(
        result.errors,
        [](const auto& e)
        { return e.find("aperture uid=") != std::string::npos; });
    CHECK(aperture_errs == 0);
  }

  SUBCASE("phase aperture UID missing from aperture_array → error")
  {
    // uid=99 is not in aperture_array → must surface as an error.
    p.simulation.phase_array.front().apertures = {Uid {51}, Uid {99}};
    auto result = validate_planning(p);
    CHECK_FALSE(result.ok());
    const auto found = std::ranges::any_of(
        result.errors,
        [](const auto& e)
        {
          return e.find("aperture uid=99") != std::string::npos
              && e.find("aperture_array") != std::string::npos;
        });
    CHECK(found);
  }

  SUBCASE("empty aperture_array → check is a no-op")
  {
    p.simulation.aperture_array.clear();
    p.simulation.phase_array.front().apertures = {Uid {51}};  // dangling
    auto result = validate_planning(p);
    // No error from check_aperture_references because the global
    // array is empty (apertures are disabled altogether).
    const auto aperture_errs = std::ranges::count_if(
        result.errors,
        [](const auto& e)
        { return e.find("aperture uid=") != std::string::npos; });
    CHECK(aperture_errs == 0);
  }

  SUBCASE("empty phase apertures → check is a no-op")
  {
    p.simulation.phase_array.front().apertures.clear();
    auto result = validate_planning(p);
    const auto aperture_errs = std::ranges::count_if(
        result.errors,
        [](const auto& e)
        { return e.find("aperture uid=") != std::string::npos; });
    CHECK(aperture_errs == 0);
  }
}

TEST_CASE(  // NOLINT
    "validate_planning - vector-schedule emin validates per stage")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // Replace scalar emin with a vector schedule.  The validator now
  // walks every stage and emits ONE summary warning per (element,
  // segment, direction) listing how many stages fail and the worst
  // offender — replaces the earlier "skip on schedule form" behavior
  // that hid real feasibility bugs in PLP-converted cases.
  p.system.reservoir_array.front().emin = std::vector<Real> {100.0, 200.0};
  p.system.reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "seep1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  // f(100) = -50 + 0.01·100 = -49 < fmin=0 — INFEASIBLE.
                  {
                      .volume = 0.0,
                      .slope = 0.01,
                      .constant = -50.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  // Warn-only — `result.ok()` still true (no errors).
  CHECK(result.ok());
  const auto seep_warns = std::ranges::count_if(
      result.warnings,
      [](const auto& w) { return w.find("seep1") != std::string::npos; });
  CHECK(seep_warns == 1);
  // The summary message names the failure count and points at the
  // worst stage.
  const auto found =
      std::ranges::any_of(result.warnings,
                          [](const auto& w)
                          {
                            return w.find("seep1") != std::string::npos
                                && w.find("fmin") != std::string::npos
                                && w.find("of 1 stages") != std::string::npos;
                          });
  CHECK(found);
}

TEST_CASE(  // NOLINT
    "validate_planning - file-schedule emin defers segment validation")
{
  auto p = make_minimal_with_reservoir_and_waterway();
  // File-schedule (string path) is the ONLY remaining "deferred" path
  // — we don't load arbitrary CSV/parquet at validation time, so the
  // segment feasibility check truly cannot evaluate.  Vector schedule
  // (covered above) now validates per-stage.
  p.system.reservoir_array.front().emin = std::string {"input/emin.csv"};
  p.system.reservoir_seepage_array = {
      {
          .uid = Uid {1},
          .name = "seep1",
          .waterway = Uid {1},
          .reservoir = Uid {1},
          .segments =
              {
                  {
                      .volume = 0.0,
                      .slope = 0.01,
                      .constant = -50.0,
                  },
              },
      },
  };
  auto result = validate_planning(p);
  CHECK(result.ok());
  // No warning: the file path can't be resolved at validation time,
  // so the segment check is genuinely deferred to the LP-build stage.
  const auto piecewise_warns = std::ranges::count_if(
      result.warnings,
      [](const auto& w) { return w.find("seep1") != std::string::npos; });
  CHECK(piecewise_warns == 0);
}

}  // namespace
