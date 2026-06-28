/**
 * @file      test_domain_rules.cpp
 * @brief     Unit tests for the electric-system commitment-repair rules
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers the isolated electric-system layer of the MIP-start pipeline: the
 * `DomainRule` interface, the `MinUpDownRule`, and the
 * `DomainRulePipeline` (composition + the default registration).  These
 * rules are solver- and LP-independent — they operate on a dense
 * raw-column-indexed value vector — so no solver is needed.
 */

#include <array>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/domain_rules.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace domain_rules_test  // NOLINT(misc-use-anonymous-namespace)
{

TEST_CASE("MinUpDownRule suppresses a premature transition")  // NOLINT
{
  // status 1,1,0,1 with min_down = 2h and 1h blocks: the single 1h OFF period
  // is shorter than min_down, so the off->on transition is suppressed — the
  // OFF run is extended and the last value flips 1 -> 0.
  std::vector<double> values {1.0, 1.0, 0.0, 1.0};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .min_up_hours = 1.0,
      .min_down_hours = 2.0,
      .status_cols = {0, 1, 2, 3},
      .durations = {1.0, 1.0, 1.0, 1.0},
  }};
  const MinUpDownRule rule;
  const int flipped =
      rule.apply(values, DomainRuleContext {.commitments = commitments});
  CHECK(flipped == 1);
  CHECK(values[0] == doctest::Approx(1.0));
  CHECK(values[1] == doctest::Approx(1.0));
  CHECK(values[2] == doctest::Approx(0.0));
  CHECK(values[3] == doctest::Approx(0.0));  // flipped 1 -> 0
}

TEST_CASE("MinUpDownRule leaves a feasible commitment untouched")  // NOLINT
{
  std::vector<double> values {1.0, 1.0, 0.0, 0.0};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .min_up_hours = 2.0,
      .min_down_hours = 2.0,
      .status_cols = {0, 1, 2, 3},
      .durations = {1.0, 1.0, 1.0, 1.0},
  }};
  const MinUpDownRule rule;
  CHECK(rule.apply(values, DomainRuleContext {.commitments = commitments})
        == 0);
}

TEST_CASE("MinUpDownRule is a no-op without run-length limits")  // NOLINT
{
  // min_up = min_down = 0 → the rule skips the unit entirely; the fast on/off
  // toggling is left as-is.
  std::vector<double> values {1.0, 0.0, 1.0, 0.0};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .status_cols = {0, 1, 2, 3},
      .durations = {1.0, 1.0, 1.0, 1.0},
  }};
  const MinUpDownRule rule;
  CHECK(rule.apply(values, DomainRuleContext {.commitments = commitments})
        == 0);
  CHECK(values[1] == doctest::Approx(0.0));  // unchanged
}

TEST_CASE("DomainRulePipeline composes rules")  // NOLINT
{
  SUBCASE("empty pipeline flips nothing")
  {
    const DomainRulePipeline pipeline;
    CHECK(pipeline.empty());
    CHECK(pipeline.size() == 0);
    std::vector<double> values {1.0, 0.0, 1.0};
    CHECK(pipeline.apply(values, DomainRuleContext {}) == 0);
  }

  SUBCASE("default pipeline registers the min-up/down + peak-injection rules")
  {
    const auto pipeline = make_default_domain_rules();
    CHECK_FALSE(pipeline.empty());
    CHECK(pipeline.size() == 2);

    std::vector<double> values {1.0, 1.0, 0.0, 1.0};
    const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
        .min_up_hours = 1.0,
        .min_down_hours = 2.0,
        .status_cols = {0, 1, 2, 3},
        .durations = {1.0, 1.0, 1.0, 1.0},
    }};
    const int flipped =
        pipeline.apply(values, DomainRuleContext {.commitments = commitments});
    CHECK(flipped == 1);
    CHECK(values[3] == doctest::Approx(0.0));  // routed through MinUpDownRule
  }

  SUBCASE("default pipeline routes through the peak-injection rule")
  {
    const auto pipeline = make_default_domain_rules();
    // status 0,0 on two blocks; block 1 is peak → seeded ON via
    // PeakInjectionRule (no commitments, so MinUpDownRule is a no-op).
    std::vector<double> values {0.0, 0.0};
    const std::array<PeakInjectionInfo, 1> injections {PeakInjectionInfo {
        .status_cols = {0, 1},
        .is_peak = {0, 1},
    }};
    const int flipped =
        pipeline.apply(values, DomainRuleContext {.injections = injections});
    CHECK(flipped == 1);
    CHECK(values[0] == doctest::Approx(0.0));  // off-peak untouched
    CHECK(values[1] == doctest::Approx(1.0));  // peak seeded ON
  }
}

TEST_CASE("PeakInjectionRule seeds storage ON during peak blocks")  // NOLINT
{
  // status 0,0,0,0 over four blocks; blocks 2 and 3 are flagged peak → the
  // rule flips the two peak blocks ON and leaves the two off-peak ones OFF.
  std::vector<double> values {0.0, 0.0, 0.0, 0.0};
  const std::array<PeakInjectionInfo, 1> injections {PeakInjectionInfo {
      .status_cols = {0, 1, 2, 3},
      .is_peak = {0, 0, 1, 1},
  }};
  const PeakInjectionRule rule;
  const int flipped =
      rule.apply(values, DomainRuleContext {.injections = injections});
  CHECK(flipped == 2);
  CHECK(values[0] == doctest::Approx(0.0));  // off-peak — untouched
  CHECK(values[1] == doctest::Approx(0.0));  // off-peak — untouched
  CHECK(values[2] == doctest::Approx(1.0));  // peak — seeded ON
  CHECK(values[3] == doctest::Approx(1.0));  // peak — seeded ON
}

TEST_CASE("PeakInjectionRule never turns a committed unit OFF")  // NOLINT
{
  // The unit is already ON at every block (the LP relaxation committed it).
  // The conservative policy only flips toward ON, so a peak block that is
  // already ON is left as-is (0 flips) and an off-peak ON stays ON.
  std::vector<double> values {1.0, 1.0};
  const std::array<PeakInjectionInfo, 1> injections {PeakInjectionInfo {
      .status_cols = {0, 1},
      .is_peak = {0, 1},  // block 1 is peak but already ON
  }};
  const PeakInjectionRule rule;
  CHECK(rule.apply(values, DomainRuleContext {.injections = injections}) == 0);
  CHECK(values[0] == doctest::Approx(1.0));
  CHECK(values[1] == doctest::Approx(1.0));
}

TEST_CASE("PeakInjectionRule is a no-op without injection data")  // NOLINT
{
  // Empty injections (the feature is disabled / the hook supplied nothing) →
  // the rule flips nothing, mirroring MinUpDownRule on a unit with no limits.
  std::vector<double> values {0.0, 0.0, 0.0};
  const PeakInjectionRule rule;
  CHECK(rule.apply(values, DomainRuleContext {}) == 0);
  CHECK(values[0] == doctest::Approx(0.0));
}

TEST_CASE("PeakInjectionRule maps to RAW columns, not block order")  // NOLINT
{
  // status_cols are RAW LP column indices, not 0..n — the rule must index the
  // value vector THROUGH status_cols.  Here the peak block's column is 5.
  std::vector<double> values {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  const std::array<PeakInjectionInfo, 1> injections {PeakInjectionInfo {
      .status_cols = {3, 5},
      .is_peak = {0, 1},
  }};
  const PeakInjectionRule rule;
  CHECK(rule.apply(values, DomainRuleContext {.injections = injections}) == 1);
  CHECK(values[3] == doctest::Approx(0.0));  // off-peak column untouched
  CHECK(values[5] == doctest::Approx(1.0));  // peak column seeded ON
}

}  // namespace domain_rules_test
