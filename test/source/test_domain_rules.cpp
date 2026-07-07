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
#include <filesystem>
#include <fstream>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/domain_rules.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_options.hpp>

using namespace gtopt;

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

  SUBCASE("default pipeline registers min-up/down + peak + commitment-logic")
  {
    const auto pipeline = make_default_domain_rules(MipStartDomainRules {});
    CHECK_FALSE(pipeline.empty());
    CHECK(pipeline.size() == 3);

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
    const auto pipeline = make_default_domain_rules(MipStartDomainRules {});
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

TEST_CASE("make_default_domain_rules honours the per-stage toggles")  // NOLINT
{
  SUBCASE("all toggles default ON → three rules")
  {
    CHECK(make_default_domain_rules(MipStartDomainRules {}).size() == 3);
  }

  SUBCASE("min_up_down=false drops one rule")
  {
    MipStartDomainRules opts;
    opts.min_up_down = false;
    CHECK(make_default_domain_rules(opts).size() == 2);
  }

  SUBCASE("peak_injection.enabled=false drops one rule")
  {
    MipStartDomainRules opts;
    opts.peak_injection.enabled = false;
    CHECK(make_default_domain_rules(opts).size() == 2);
  }

  SUBCASE("commitment_logic=false drops one rule")
  {
    MipStartDomainRules opts;
    opts.commitment_logic = false;
    CHECK(make_default_domain_rules(opts).size() == 2);
  }

  SUBCASE("all toggles OFF → empty pipeline")
  {
    MipStartDomainRules opts;
    opts.min_up_down = false;
    opts.commitment_logic = false;
    opts.peak_injection.enabled = false;
    CHECK(make_default_domain_rules(opts).empty());
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

TEST_CASE(
    "SeedCommitmentRule overwrites matched (generator,block) status")  // NOLINT
{
  // Generator uid=7, two blocks (uids 10, 11).  The rounded candidate has them
  // 0,1; the seed says 10→ON, 11→OFF, so both flip to the seed.
  std::vector<double> values {0.0, 1.0};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .uid = Uid {7},
      .status_cols = {0, 1},
      .block_uids = {Uid {10}, Uid {11}},
  }};
  SeedCommitmentMap seed;
  seed[seed_commitment_key(Uid {7}, Uid {10})] = 1.0;
  seed[seed_commitment_key(Uid {7}, Uid {11})] = 0.0;
  const SeedCommitmentRule rule {seed};
  const int flipped =
      rule.apply(values, DomainRuleContext {.commitments = commitments});
  CHECK(flipped == 2);
  CHECK(values[0] == doctest::Approx(1.0));  // block 10 seeded ON
  CHECK(values[1] == doctest::Approx(0.0));  // block 11 seeded OFF
}

TEST_CASE(
    "SeedCommitmentRule leaves unmatched elements/blocks untouched")  // NOLINT
{
  // The seed knows only generator 7 / block 10.  Generator 7 / block 99 and a
  // whole other generator keep their rounded value — the "existing elements"
  // behaviour that lets a partial seed apply safely.
  std::vector<double> values {0.0, 0.0, 1.0};
  const std::array<CommitmentRunInfo, 2> commitments {
      CommitmentRunInfo {
          .uid = Uid {7},
          .status_cols = {0, 1},
          .block_uids = {Uid {10}, Uid {99}},  // block 99 absent from seed
      },
      CommitmentRunInfo {
          .uid = Uid {8},  // generator absent from seed
          .status_cols = {2},
          .block_uids = {Uid {10}},
      },
  };
  SeedCommitmentMap seed;
  seed[seed_commitment_key(Uid {7}, Uid {10})] = 1.0;
  const SeedCommitmentRule rule {seed};
  const int flipped =
      rule.apply(values, DomainRuleContext {.commitments = commitments});
  CHECK(flipped == 1);
  CHECK(values[0] == doctest::Approx(1.0));  // matched → seeded ON
  CHECK(values[1] == doctest::Approx(0.0));  // block 99 unmatched → kept
  CHECK(values[2] == doctest::Approx(1.0));  // generator 8 unmatched → kept
}

TEST_CASE("SeedCommitmentRule maps through RAW status_cols")  // NOLINT
{
  // status_cols are RAW LP columns (3, 5), not 0..n — the rule must index the
  // value vector THROUGH status_cols, like every other rule.
  std::vector<double> values {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .uid = Uid {7},
      .status_cols = {3, 5},
      .block_uids = {Uid {10}, Uid {11}},
  }};
  SeedCommitmentMap seed;
  seed[seed_commitment_key(Uid {7}, Uid {11})] = 1.0;
  const SeedCommitmentRule rule {seed};
  CHECK(rule.apply(values, DomainRuleContext {.commitments = commitments})
        == 1);
  CHECK(values[3] == doctest::Approx(0.0));  // block 10 not in seed → kept
  CHECK(values[5] == doctest::Approx(1.0));  // block 11 seeded ON at RAW col 5
}

TEST_CASE(
    "SeedCommitmentRule is a no-op on empty seed or unknown uid")  // NOLINT
{
  std::vector<double> values {0.0, 1.0};
  SUBCASE("empty seed flips nothing")
  {
    const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
        .uid = Uid {7},
        .status_cols = {0, 1},
        .block_uids = {Uid {10}, Uid {11}},
    }};
    const SeedCommitmentRule rule {SeedCommitmentMap {}};
    CHECK(rule.apply(values, DomainRuleContext {.commitments = commitments})
          == 0);
  }
  SUBCASE("a unit with unknown_uid is skipped even if a key matches")
  {
    const std::array<CommitmentRunInfo, 1> anon {CommitmentRunInfo {
        .status_cols = {0, 1},  // uid defaults to unknown_uid
        .block_uids = {Uid {10}, Uid {11}},
    }};
    SeedCommitmentMap seed;
    seed[seed_commitment_key(unknown_uid, Uid {10})] = 1.0;
    const SeedCommitmentRule rule {seed};
    CHECK(rule.apply(values, DomainRuleContext {.commitments = anon}) == 0);
  }
}

TEST_CASE("make_default_domain_rules registers the seed rule FIRST")  // NOLINT
{
  SUBCASE("empty seed → legacy three-rule pipeline")
  {
    CHECK(
        make_default_domain_rules(MipStartDomainRules {}, SeedCommitmentMap {})
            .size()
        == 3);
  }
  SUBCASE("non-empty seed prepends a fourth rule")
  {
    SeedCommitmentMap seed;
    seed[seed_commitment_key(Uid {1}, Uid {1})] = 1.0;
    CHECK(make_default_domain_rules(MipStartDomainRules {}, std::move(seed))
              .size()
          == 4);
  }
}

TEST_CASE("load_seed_commitment reads a CSV through Arrow")  // NOLINT
{
  const auto path =
      (std::filesystem::temp_directory_path() / "gtopt_seed_rule_test.csv")
          .string();
  {
    std::ofstream ofs(path);
    ofs << "generator_uid,block_uid,u\n";
    ofs << "7,10,1.0\n";
    ofs << "7,11,0\n";
    ofs << "8,10,0.75\n";
  }
  const auto seed = load_seed_commitment(path);
  std::filesystem::remove(path);
  REQUIRE(seed.has_value());
  const auto get = [&](Uid g, Uid b) -> double
  {
    const auto it = seed->find(seed_commitment_key(g, b));
    return it != seed->end() ? it->second : -999.0;
  };
  CHECK(seed->size() == 3);
  CHECK(get(Uid {7}, Uid {10}) == doctest::Approx(1.0));
  CHECK(get(Uid {7}, Uid {11}) == doctest::Approx(0.0));
  CHECK(get(Uid {8}, Uid {10}) == doctest::Approx(0.75));
}

TEST_CASE("load_seed_commitment errors on a missing file")  // NOLINT
{
  CHECK_FALSE(load_seed_commitment("/nonexistent/gtopt_seed_does_not_exist.csv")
                  .has_value());
}

}  // namespace domain_rules_test
