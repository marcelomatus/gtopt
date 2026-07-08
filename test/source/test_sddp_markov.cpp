// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_markov.cpp
 * @brief     Unit tests for Markov-chain SDDP configuration and pricing
 *            (`cut_sharing_mode = markov`, opt-in / experimental).
 * @date      2026-07-08
 *
 * Covers the pure configuration layer:
 *   * `make_markov_config` M inference,
 *   * `validate_markov_config` acceptance / every rejection branch,
 *   * `markov_alpha_weights` — the theorem-MK1 pricing
 *     `w_{s,m'} = p_s·P[m(s)][m'] / pi_{m'}` and its degenerate
 *     equivalences (M = N singleton → w = p_s; M = 1 → single column
 *     priced p_s), plus the invariance `Σ_{m'} w_{s,m'}·pi_{m'} = p_s`,
 *   * SDDP setup behaviour: hard validation error on a bad config,
 *     WARN on multi-scene states, silence on singleton states.
 *
 * The oracle-gated cut-validity properties live in
 * `test_sddp_cut_oracle.cpp`; bounds invariants in
 * `test_sddp_bounds_sanity.cpp`; the persistence round-trip in
 * `test_sddp_cut_io.cpp`.  Derivation: `docs/formulation/sddp-markov.md`.
 */

#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_enums.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/utils.hpp>

#include "log_capture.hpp"
#include "sddp_helpers.hpp"

using namespace gtopt;

namespace
{

[[nodiscard]] MarkovChainConfig two_state_singleton_config(double p00 = 0.5,
                                                           double p10 = 0.5)
{
  return make_markov_config(
      {
          0,
          1,
      },
      {
          p00,
          1.0 - p00,
          p10,
          1.0 - p10,
      });
}

}  // namespace

// ─── make_markov_config ─────────────────────────────────────────────────────

TEST_CASE("make_markov_config infers M from the transition length")  // NOLINT
{
  SUBCASE("empty → M = 0 (empty config)")
  {
    const auto config = make_markov_config({}, {});
    CHECK(config.empty());
    CHECK(config.num_states == 0);
  }

  SUBCASE("1×1")
  {
    const auto config = make_markov_config(
        {
            0,
            0,
        },
        {
            1.0,
        });
    CHECK(config.num_states == 1);
    CHECK(config.probability(0, 0) == doctest::Approx(1.0));
  }

  SUBCASE("2×2 row-major layout")
  {
    const auto config = two_state_singleton_config(0.7, 0.2);
    CHECK(config.num_states == 2);
    CHECK(config.probability(0, 0) == doctest::Approx(0.7));
    CHECK(config.probability(0, 1) == doctest::Approx(0.3));
    CHECK(config.probability(1, 0) == doctest::Approx(0.2));
    CHECK(config.probability(1, 1) == doctest::Approx(0.8));
  }
}

// ─── validate_markov_config ─────────────────────────────────────────────────

TEST_CASE("validate_markov_config accepts a well-formed config")  // NOLINT
{
  const auto config = two_state_singleton_config();
  CHECK_FALSE(validate_markov_config(config, 2).has_value());
}

TEST_CASE("validate_markov_config rejection branches")  // NOLINT
{
  SUBCASE("empty config (markov_transition missing)")
  {
    const auto err = validate_markov_config(make_markov_config({}, {}), 2);
    REQUIRE(err.has_value());
    CHECK(err->contains("num_states"));
  }

  SUBCASE("non-square transition length")
  {
    auto config = two_state_singleton_config();
    config.transition.push_back(0.0);  // length 5 — not M*M for any M
    config.num_states = 2;
    const auto err = validate_markov_config(config, 2);
    REQUIRE(err.has_value());
    CHECK(err->contains("perfect square"));
  }

  SUBCASE("wrong markov_states length")
  {
    const auto err = validate_markov_config(two_state_singleton_config(), 3);
    REQUIRE(err.has_value());
    CHECK(err->contains("scene"));
  }

  SUBCASE("assignment out of range")
  {
    auto config = two_state_singleton_config();
    config.state_of_scene = {
        0,
        2,
    };
    const auto err = validate_markov_config(config, 2);
    REQUIRE(err.has_value());
    CHECK(err->contains("out of range"));
  }

  SUBCASE("empty state (zero mass would divide the pricing)")
  {
    auto config = two_state_singleton_config();
    config.state_of_scene = {
        0,
        0,
    };
    const auto err = validate_markov_config(config, 2);
    REQUIRE(err.has_value());
    CHECK(err->contains("no assigned scene"));
  }

  SUBCASE("row sum != 1")
  {
    const auto err = validate_markov_config(make_markov_config(
                                                {
                                                    0,
                                                    1,
                                                },
                                                {
                                                    0.5,
                                                    0.4,
                                                    0.5,
                                                    0.5,
                                                }),
                                            2);
    REQUIRE(err.has_value());
    CHECK(err->contains("sums to"));
  }

  SUBCASE("negative entry")
  {
    const auto err = validate_markov_config(make_markov_config(
                                                {
                                                    0,
                                                    1,
                                                },
                                                {
                                                    1.2,
                                                    -0.2,
                                                    0.5,
                                                    0.5,
                                                }),
                                            2);
    REQUIRE(err.has_value());
    CHECK(err->contains(">= 0"));
  }
}

// ─── markov_alpha_weights (theorem MK1 pricing) ─────────────────────────────

TEST_CASE("markov_alpha_weights — theorem MK1 pricing")  // NOLINT
{
  // 2-scene fixture with p = (0.7, 0.3): the weights below exercise the
  // raw-mass scale invariance of w = p_s·P[m(s)][m']/pi_{m'}.
  auto planning = make_2scene_3phase_hydro_planning(0.7, 0.3);
  PlanningLP plp(std::move(planning));
  const auto& sim = plp.simulation();

  SUBCASE("degenerate M = N: transition rows = scene probabilities → w = p_s")
  {
    // Singleton states with P rows equal to the (normalized) scene
    // probabilities: pi_{m'} = p_{r(m')} cancels P's p_r, leaving the
    // multicut M4 pricing w_{s,m'} = p_s for every column
    // (docs/formulation/sddp-markov.md §4.1).
    const auto config = two_state_singleton_config(0.7, 0.7);
    REQUIRE_FALSE(validate_markov_config(config, 2).has_value());

    const auto w0 = markov_alpha_weights(sim, config, SceneIndex {0});
    const auto w1 = markov_alpha_weights(sim, config, SceneIndex {1});
    REQUIRE(w0.size() == 2);
    REQUIRE(w1.size() == 2);
    CHECK(w0[0] == doctest::Approx(0.7));
    CHECK(w0[1] == doctest::Approx(0.7));
    CHECK(w1[0] == doctest::Approx(0.3));
    CHECK(w1[1] == doctest::Approx(0.3));
  }

  SUBCASE("degenerate M = 1: single column priced p_s")
  {
    // pi_0 = Σ_r p_r = 1, P = [1] → w_{s,0} = p_s: the single-cut
    // expected-SDDP layout (docs/formulation/sddp-markov.md §4.2).
    const auto config = make_markov_config(
        {
            0,
            0,
        },
        {
            1.0,
        });
    REQUIRE_FALSE(validate_markov_config(config, 2).has_value());

    const auto w0 = markov_alpha_weights(sim, config, SceneIndex {0});
    const auto w1 = markov_alpha_weights(sim, config, SceneIndex {1});
    REQUIRE(w0.size() == 1);
    REQUIRE(w1.size() == 1);
    CHECK(w0[0] == doctest::Approx(0.7));
    CHECK(w1[0] == doctest::Approx(0.3));
  }

  SUBCASE("identity transition: w_{s,m(s)} = p_s/pi_{m(s)}, off-state 0")
  {
    const auto config = two_state_singleton_config(1.0, 0.0);
    REQUIRE_FALSE(validate_markov_config(config, 2).has_value());

    const auto w0 = markov_alpha_weights(sim, config, SceneIndex {0});
    const auto w1 = markov_alpha_weights(sim, config, SceneIndex {1});
    // Scene 0 (state 0): w_{0,0} = 0.7·1/0.7 = 1, w_{0,1} = 0.
    CHECK(w0[0] == doctest::Approx(1.0));
    CHECK(w0[1] == doctest::Approx(0.0));
    // Scene 1 (state 1): w_{1,0} = 0, w_{1,1} = 0.3·1/0.3 = 1.
    CHECK(w1[0] == doctest::Approx(0.0));
    CHECK(w1[1] == doctest::Approx(1.0));
  }

  SUBCASE("invariance: Σ_{m'} w_{s,m'}·pi_{m'} = p_s (row-stochastic P)")
  {
    const auto config = two_state_singleton_config(0.6, 0.2);
    REQUIRE_FALSE(validate_markov_config(config, 2).has_value());

    // Singleton states → pi = (p_0, p_1) = (0.7, 0.3).
    const std::vector<double> pi = {
        0.7,
        0.3,
    };
    const std::vector<double> probs = {
        0.7,
        0.3,
    };
    for (const auto s : iota_range<SceneIndex>(0, 2)) {
      const auto w = markov_alpha_weights(sim, config, s);
      double folded = 0.0;
      for (std::size_t m = 0; m < w.size(); ++m) {
        folded += w[m] * pi[m];
      }
      CHECK(folded == doctest::Approx(probs[static_cast<std::size_t>(s)]));
    }
  }
}

// ─── SDDP setup: validation + WARN behaviour ────────────────────────────────

TEST_CASE(
    "SDDP markov — invalid config fails solve with a markov "
    "error")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.cut_sharing = CutSharingMode::markov;
  opts.enable_api = false;
  // Row 0 sums to 0.9 → hard validation error at initialize_solver.
  opts.markov = make_markov_config(
      {
          0,
          1,
      },
      {
          0.5,
          0.4,
          0.5,
          0.5,
      });

  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE_FALSE(results.has_value());
  CHECK(results.error().message.contains("markov"));
}

TEST_CASE("SDDP markov — missing config fails solve")  // NOLINT
{
  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.cut_sharing = CutSharingMode::markov;
  opts.enable_api = false;
  // opts.markov left empty — markov mode without a configuration.

  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE_FALSE(results.has_value());
  CHECK(results.error().message.contains("markov"));
}

TEST_CASE(
    "SDDP markov WARN — fires for multi-scene states "
    "(valid-but-loose)")  // NOLINT
{
  gtopt::test::LogCapture logs;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.cut_sharing = CutSharingMode::markov;
  opts.enable_api = false;
  opts.apertures = std::vector<Uid> {};
  // M = 1 with two scenes: state 0 has multiplicity 2 → theorem MK1 §3
  // WARN (cut family converges to the within-state MAX, not the sum).
  opts.markov = make_markov_config(
      {
          0,
          0,
      },
      {
          1.0,
      });

  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK(logs.contains("scenes sharing"));
}

TEST_CASE("SDDP markov WARN — silent for singleton states")  // NOLINT
{
  gtopt::test::LogCapture logs;

  auto planning = make_2scene_3phase_hydro_planning(0.5, 0.5);
  PlanningLP plp(std::move(planning));

  SDDPOptions opts;
  opts.max_iterations = 1;
  opts.cut_sharing = CutSharingMode::markov;
  opts.enable_api = false;
  opts.apertures = std::vector<Uid> {};
  opts.markov = two_state_singleton_config();

  SDDPMethod sddp(plp, opts);
  auto results = sddp.solve();
  REQUIRE(results.has_value());
  CHECK_FALSE(logs.contains("scenes sharing"));
}

// ─── Enum wiring ────────────────────────────────────────────────────────────

TEST_CASE("CutSharingMode markov parses and round-trips")  // NOLINT
{
  CHECK(parse_cut_sharing_mode("markov") == CutSharingMode::markov);
  CHECK(enum_name(CutSharingMode::markov) == "markov");
}
