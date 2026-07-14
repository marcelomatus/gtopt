/**
 * @file      test_domain_rules.cpp
 * @brief     Unit tests for the MIP-start seed-application rule + pipeline
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Covers the `DomainRule` interface, the `SeedCommitmentRule` and the
 * `DomainRulePipeline`.  The in-tree commitment-REPAIR rules (min-up/down,
 * v/w logic, peak injection) were removed 2026-07-14 — repair belongs with
 * the seed producer (`scripts/gtopt_warmstart/build_full_seed.py`); see
 * domain_rules.hpp.
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/domain_rules.hpp>
#include <gtopt/mip_start.hpp>
#include <gtopt/monolithic_options.hpp>

using namespace gtopt;

namespace domain_rules_test  // NOLINT(misc-use-anonymous-namespace)
{

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

TEST_CASE("make_default_domain_rules registers only the seed rule")  // NOLINT
{
  SUBCASE("empty seed -> empty pipeline (rounded candidate passes through)")
  {
    CHECK(make_default_domain_rules(SeedCommitmentMap {}).empty());
  }
  SUBCASE("non-empty seed -> a single SeedCommitmentRule")
  {
    SeedCommitmentMap seed;
    seed[seed_commitment_key(Uid {1}, Uid {1})] = 1.0;
    CHECK(make_default_domain_rules(std::move(seed)).size() == 1);
  }
}

TEST_CASE("DomainRulePipeline composes rules and reaches a fixpoint")  // NOLINT
{
  // Two seed rules over the same unit: the second overwrites the first, and
  // a second pass flips nothing (fixpoint exit).
  std::vector<double> values {0.0};
  const std::array<CommitmentRunInfo, 1> commitments {CommitmentRunInfo {
      .uid = Uid {7},
      .status_cols = {0},
      .block_uids = {Uid {10}},
  }};
  SeedCommitmentMap on;
  on[seed_commitment_key(Uid {7}, Uid {10})] = 1.0;
  SeedCommitmentMap off;
  off[seed_commitment_key(Uid {7}, Uid {10})] = 0.0;
  DomainRulePipeline pipeline;
  pipeline.add(std::make_unique<SeedCommitmentRule>(on));
  pipeline.add(std::make_unique<SeedCommitmentRule>(off));
  CHECK(pipeline.size() == 2);
  const int flipped = pipeline.apply(
      values, DomainRuleContext {.commitments = commitments}, /*max_passes=*/4);
  // Pass 1: ON flips 0->1, OFF flips 1->0.  Pass 2: ON flips again, OFF
  // flips again.  ... the pipeline caps at max_passes when rules disagree.
  CHECK(flipped >= 2);
  CHECK(values[0] == doctest::Approx(0.0));  // last rule wins
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
