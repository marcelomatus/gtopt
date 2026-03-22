// SPDX-License-Identifier: BSD-3-Clause

/**
 * @file      test_sddp_aperture_functions.hpp
 * @brief     Tests for build_effective_apertures and build_synthetic_apertures
 * @date      2026-03-22
 */

#include <doctest/doctest.h>
#include <gtopt/aperture.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/sddp_aperture.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── build_effective_apertures ──────────────────────────────────────────────

TEST_CASE(
    "build_effective_apertures — empty phase_apertures uses all active")  // NOLINT
{
  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
      {
          .uid = Uid {3},
          .source_scenario = Uid {30},
      },
  };

  const auto result = build_effective_apertures(defs, {});
  REQUIRE(result.size() == 3);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 1);
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[1].count == 1);
  CHECK(result[2].aperture.get().uid == Uid {3});
  CHECK(result[2].count == 1);
}

TEST_CASE(
    "build_effective_apertures — inactive apertures are excluded")  // NOLINT
{
  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .active = false,
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
  };

  const auto result = build_effective_apertures(defs, {});
  REQUIRE(result.size() == 1);
  CHECK(result[0].aperture.get().uid == Uid {2});
}

TEST_CASE(
    "build_effective_apertures — deduplicates phase_apertures with counts")  // NOLINT
{
  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
      {
          .uid = Uid {3},
          .source_scenario = Uid {30},
      },
  };

  // Phase apertures: [1, 2, 2, 3, 3, 3]
  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {2},
      Uid {2},
      Uid {3},
      Uid {3},
      Uid {3},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 3);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 1);
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[1].count == 2);
  CHECK(result[2].aperture.get().uid == Uid {3});
  CHECK(result[2].count == 3);
}

TEST_CASE(
    "build_effective_apertures — preserves first-appearance order")  // NOLINT
{
  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
      {
          .uid = Uid {3},
          .source_scenario = Uid {30},
      },
  };

  // Phase apertures in reverse order
  const std::vector<Uid> phase_aps {
      Uid {3},
      Uid {2},
      Uid {1},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 3);
  CHECK(result[0].aperture.get().uid == Uid {3});
  CHECK(result[1].aperture.get().uid == Uid {2});
  CHECK(result[2].aperture.get().uid == Uid {1});
}

TEST_CASE(
    "build_effective_apertures — unknown UIDs in phase_apertures are skipped")  // NOLINT
{
  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .source_scenario = Uid {10},
      },
  };

  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {99},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 1);
  CHECK(result[0].aperture.get().uid == Uid {1});
  CHECK(result[0].count == 1);
}

TEST_CASE(
    "build_effective_apertures — empty defs yields empty result")  // NOLINT
{
  const std::vector<Aperture> defs {};
  const auto result = build_effective_apertures(defs, {});
  CHECK(result.empty());
}

TEST_CASE(
    "build_effective_apertures — inactive apertures in phase list are skipped")  // NOLINT
{
  const std::vector<Aperture> defs {
      {
          .uid = Uid {1},
          .active = false,
          .source_scenario = Uid {10},
      },
      {
          .uid = Uid {2},
          .source_scenario = Uid {20},
      },
  };

  const std::vector<Uid> phase_aps {
      Uid {1},
      Uid {2},
  };

  const auto result = build_effective_apertures(defs, phase_aps);
  REQUIRE(result.size() == 1);
  CHECK(result[0].aperture.get().uid == Uid {2});
}

// ─── build_synthetic_apertures ──────────────────────────────────────────────

TEST_CASE(
    "build_synthetic_apertures — creates N apertures with equal probability")  // NOLINT
{
  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {
          Scenario {
              .uid = Uid {10},
          },
          ScenarioIndex {0},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {20},
          },
          ScenarioIndex {1},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {30},
          },
          ScenarioIndex {2},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {40},
          },
          ScenarioIndex {3},
      },
  };

  const auto result = build_synthetic_apertures(scenarios, 4);
  REQUIRE(result.size() == 4);

  for (const auto& ap : result) {
    CHECK(ap.probability_factor.value_or(0.0) == doctest::Approx(0.25));
    CHECK(ap.uid == ap.source_scenario);
  }

  CHECK(result[0].uid == Uid {10});
  CHECK(result[1].uid == Uid {20});
  CHECK(result[2].uid == Uid {30});
  CHECK(result[3].uid == Uid {40});
}

TEST_CASE("build_synthetic_apertures — caps at scenario count")  // NOLINT
{
  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {
          Scenario {
              .uid = Uid {1},
          },
          ScenarioIndex {0},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {2},
          },
          ScenarioIndex {1},
      },
  };

  const auto result = build_synthetic_apertures(scenarios, 10);
  REQUIRE(result.size() == 2);
  CHECK(result[0].probability_factor.value_or(0.0) == doctest::Approx(0.5));
  CHECK(result[1].probability_factor.value_or(0.0) == doctest::Approx(0.5));
}

TEST_CASE(
    "build_synthetic_apertures — single aperture gets probability 1.0")  // NOLINT
{
  const std::vector<ScenarioLP> scenarios {
      ScenarioLP {
          Scenario {
              .uid = Uid {5},
          },
          ScenarioIndex {0},
      },
      ScenarioLP {
          Scenario {
              .uid = Uid {6},
          },
          ScenarioIndex {1},
      },
  };

  const auto result = build_synthetic_apertures(scenarios, 1);
  REQUIRE(result.size() == 1);
  CHECK(result[0].uid == Uid {5});
  CHECK(result[0].source_scenario == Uid {5});
  CHECK(result[0].probability_factor.value_or(0.0) == doctest::Approx(1.0));
}

// ─── ApertureValueFn concept tests ──────────────────────────────────────────

TEST_CASE("ApertureValueFn — lambda returning value")  // NOLINT
{
  const ApertureValueFn fn = [](StageUid /*st*/,
                                BlockUid /*bl*/) -> std::optional<double>
  { return 42.0; };

  const auto val = fn(StageUid {0}, BlockUid {0});
  REQUIRE(val.has_value());
  CHECK(val.value_or(0.0) == doctest::Approx(42.0));
}

TEST_CASE("ApertureValueFn — lambda returning nullopt")  // NOLINT
{
  const ApertureValueFn fn = [](StageUid /*st*/,
                                BlockUid /*bl*/) -> std::optional<double>
  { return std::nullopt; };

  const auto val = fn(StageUid {0}, BlockUid {0});
  CHECK_FALSE(val.has_value());
}

TEST_CASE("ApertureValueFn — cache-backed lambda")  // NOLINT
{
  // Simulate a cache lookup pattern
  std::map<std::pair<int, int>, double> cache {
      {
          {0, 0},
          100.0,
      },
      {
          {0, 1},
          200.0,
      },
      {
          {1, 0},
          300.0,
      },
  };

  const ApertureValueFn fn = [&cache](StageUid st,
                                      BlockUid bl) -> std::optional<double>
  {
    auto key = std::pair {Index {st}, Index {bl}};
    auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
    return std::nullopt;
  };

  CHECK(fn(StageUid {0}, BlockUid {0}).value_or(0.0) == doctest::Approx(100.0));
  CHECK(fn(StageUid {0}, BlockUid {1}).value_or(0.0) == doctest::Approx(200.0));
  CHECK(fn(StageUid {1}, BlockUid {0}).value_or(0.0) == doctest::Approx(300.0));
  CHECK_FALSE(fn(StageUid {1}, BlockUid {1}).has_value());
}

// ─── ApertureCutResult ──────────────────────────────────────────────────────

TEST_CASE("ApertureCutResult default construction")  // NOLINT
{
  const ApertureCutResult result;
  CHECK(result.weight == doctest::Approx(0.0));
  CHECK_FALSE(result.feasible);
  CHECK(result.status == 0);
  CHECK_FALSE(result.cut.has_value());
}

}  // namespace
