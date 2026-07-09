// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_forward_sampling.cpp
 * @brief     ForwardSamplingMode wiring + deterministic-draw unit tests
 * @date      2026-07-08
 *
 * Covers the `forward_sampling_mode` option (persistent | resampled):
 *
 *  * `sample_weighted_index` — the deterministic splitmix64-based
 *    probability-weighted draw: pure function of the key, empirical
 *    frequencies track the weights, degenerate weights fall back to a
 *    uniform draw, single-entry always returns 0.
 *  * persistent ≡ resampled on IDENTICAL scenes — with identical
 *    dynamics every drawn realization carries the same data, so the
 *    resampled forward pass must reproduce the persistent LB/UB
 *    sequences (the hard byte-identity requirement on the default
 *    path, observed through the bounds).
 *  * JSON round-trip + enum parsing for the new option.
 *
 * The oracle-tier validity cases (strict on identical dynamics,
 * WARN-only on heterogeneous) live in `test_sddp_cut_oracle.cpp`.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_sddp_options.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_options.hpp>
#include <gtopt/sddp_types.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;

TEST_CASE("sample_weighted_index — deterministic weighted draw")  // NOLINT
{
  const std::vector<double> probs = {0.7, 0.3};

  SUBCASE("pure function of the key")
  {
    for (uint64_t it = 0; it < 16; ++it) {
      const auto a = sample_weighted_index(probs, it, 1, 2);
      const auto b = sample_weighted_index(probs, it, 1, 2);
      CHECK(a == b);
      CHECK(a < probs.size());
    }
  }

  SUBCASE("empirical frequency tracks the probabilities")
  {
    constexpr uint64_t kDraws = 4000;
    uint64_t n0 = 0;
    for (uint64_t it = 0; it < kDraws; ++it) {
      if (sample_weighted_index(probs, it, 0, 1) == 0) {
        ++n0;
      }
    }
    const double f0 = static_cast<double>(n0) / static_cast<double>(kDraws);
    CHECK(f0 == doctest::Approx(0.7).epsilon(0.06));
  }

  SUBCASE("unnormalized weights draw like their normalization")
  {
    // 6/4 must draw exactly like 0.6/0.4 for every key (the walk
    // normalizes internally).
    const std::vector<double> raw = {6.0, 4.0};
    const std::vector<double> norm = {0.6, 0.4};
    for (uint64_t it = 0; it < 64; ++it) {
      CHECK(sample_weighted_index(raw, it, 3, 7)
            == sample_weighted_index(norm, it, 3, 7));
    }
  }

  SUBCASE("degenerate weights fall back to a uniform draw")
  {
    const std::vector<double> zeros = {0.0, 0.0, 0.0};
    std::array<uint64_t, 3> counts {};
    constexpr uint64_t kDraws = 3000;
    for (uint64_t it = 0; it < kDraws; ++it) {
      const auto k = sample_weighted_index(zeros, it, 0, 1);
      REQUIRE(k < counts.size());
      ++counts[k];
    }
    for (const auto c : counts) {
      CHECK(static_cast<double>(c) / static_cast<double>(kDraws)
            == doctest::Approx(1.0 / 3.0).epsilon(0.15));
    }
  }

  SUBCASE("single entry / empty span always draw 0")
  {
    const std::vector<double> one = {1.0};
    const std::vector<double> none;
    for (uint64_t it = 0; it < 4; ++it) {
      CHECK(sample_weighted_index(one, it, 3, 5) == 0);
      CHECK(sample_weighted_index(none, it, 3, 5) == 0);
    }
  }

  SUBCASE("phase decorrelates the draw")
  {
    // Not every phase may flip the outcome, but SOME phase within a
    // small window must — otherwise the key ignores the phase.
    bool any_diff = false;
    const auto base = sample_weighted_index(probs, 7, 0, 1);
    for (uint64_t ph = 2; ph < 64 && !any_diff; ++ph) {
      any_diff = sample_weighted_index(probs, 7, 0, ph) != base;
    }
    CHECK(any_diff);
  }
}

TEST_CASE(  // NOLINT
    "SDDP forward sampling — resampled reproduces persistent bounds on "
    "identical scenes")
{
  const auto run = [](ForwardSamplingMode mode)
  {
    auto planning = make_2scene_3phase_hydro_planning(0.6, 0.4);
    PlanningLP plp(std::move(planning));

    SDDPOptions opts;
    opts.max_iterations = 6;
    opts.convergence_tol = 1.0e-9;  // keep iterating — no early exit
    opts.stationary_tol = 0.0;
    opts.cut_sharing = CutSharingMode::multicut;
    opts.forward_sampling = mode;
    opts.apertures = std::vector<Uid> {};  // pure Benders backward
    opts.enable_api = false;

    SDDPMethod sddp(plp, opts);
    auto results = sddp.solve();
    REQUIRE(results.has_value());
    REQUIRE_FALSE(results->empty());
    return *results;
  };

  const auto persistent = run(ForwardSamplingMode::persistent);
  const auto resampled = run(ForwardSamplingMode::resampled);

  // Identical dynamics: every drawn realization pins the SAME bound
  // values, so the resampled run must track the persistent one
  // iteration by iteration (both LB and UB).
  const auto n = std::min(persistent.size(), resampled.size());
  REQUIRE(n >= 2);
  for (std::size_t i = 0; i < n; ++i) {
    INFO("pos=",
         i,
         " persistent UB=",
         persistent[i].upper_bound,
         " resampled UB=",
         resampled[i].upper_bound);
    CHECK(resampled[i].upper_bound
          == doctest::Approx(persistent[i].upper_bound).epsilon(1.0e-6));
    CHECK(resampled[i].lower_bound
          == doctest::Approx(persistent[i].lower_bound).epsilon(1.0e-6));
  }
}

TEST_CASE("ForwardSamplingMode — enum + JSON wiring")  // NOLINT
{
  SUBCASE("enum entries")
  {
    CHECK(enum_from_name<ForwardSamplingMode>("persistent")
          == ForwardSamplingMode::persistent);
    CHECK(enum_from_name<ForwardSamplingMode>("resampled")
          == ForwardSamplingMode::resampled);
    CHECK_FALSE(enum_from_name<ForwardSamplingMode>("bogus").has_value());
    CHECK(enum_name(ForwardSamplingMode::persistent) == "persistent");
    CHECK(enum_name(ForwardSamplingMode::resampled) == "resampled");
  }

  SUBCASE("SddpOptions default is unset")
  {
    const SddpOptions opts {};
    CHECK_FALSE(opts.forward_sampling_mode.has_value());
  }

  SUBCASE("JSON parse + round-trip")
  {
    const std::string_view js = R"({"forward_sampling_mode": "resampled"})";
    const auto so = daw::json::from_json<SddpOptions>(js);
    CHECK((so.forward_sampling_mode
           && *so.forward_sampling_mode == ForwardSamplingMode::resampled));

    const std::string round = daw::json::to_json(so);
    const auto rt = daw::json::from_json<SddpOptions>(std::string_view {round});
    CHECK((rt.forward_sampling_mode
           && *rt.forward_sampling_mode == ForwardSamplingMode::resampled));
  }

  SUBCASE("JSON without the field stays unset")
  {
    const std::string_view js = R"({"max_iterations": 3})";
    const auto so = daw::json::from_json<SddpOptions>(js);
    CHECK_FALSE(so.forward_sampling_mode.has_value());
  }

  SUBCASE("merge: incoming wins")
  {
    SddpOptions base {};
    SddpOptions over {};
    over.forward_sampling_mode = ForwardSamplingMode::resampled;
    base.merge(std::move(over));
    CHECK((base.forward_sampling_mode
           && *base.forward_sampling_mode == ForwardSamplingMode::resampled));
  }
}
