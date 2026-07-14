/**
 * @file      domain_rules.hpp
 * @brief     Domain rules that shape a candidate integer commitment
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The MIP-start pipeline has two cleanly separated concerns:
 *
 *   1. **Rounding** — generic, domain-agnostic: round each relaxed integer
 *      column to {0,1} by a threshold (`detail::round_with_threshold`).  Any
 *      solver / any problem can do this; it knows nothing about power systems.
 *   2. **Seed application** — THIS file: overlay an externally-supplied
 *      commitment seed onto the candidate by semantic (generator, block)
 *      identity (`SeedCommitmentRule`).
 *
 * Commitment-feasibility REPAIR is deliberately NOT done here.  The in-tree
 * repair rules (min-up/down run mending, v/w logic, peak-injection seeding)
 * were removed 2026-07-14: they operated blind to the constraint families
 * that actually reject a start under a strict backend feasibility check
 * (PLEXOS order pairs, config-exclusivity/implication rows, maintenance
 * pmax-below-pmin windows — see the CEN 2026-04-12 IIS campaign), so they
 * could flip large fractions of a seed without making it feasible.  Repair
 * belongs with the SEED PRODUCER, which sees the case data and user
 * constraints: `scripts/gtopt_warmstart/build_full_seed.py` repairs to a
 * verified fixpoint before the CSV is written.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/// One commitment unit's status (u) columns in chronological order, with the
/// unit's generator identity and per-block uids.  The domain input the seed
/// rule needs — built by the `SystemLP::resolve` hook (one entry per
/// (scenario, commitment)).  `status_cols[t]` is a RAW column index;
/// `block_uids[t]` is the block half of the semantic (generator, block) key.
struct CommitmentRunInfo
{
  /// UID of the generator this commitment belongs to.  Together with the
  /// per-block `block_uids`, it is the semantic (generator, block) key an
  /// external-commitment seed (`SeedCommitmentRule`) matches on — so a seed
  /// from another day / another tool lines up by identity, not raw column
  /// index.  `unknown_uid` when unset (the seed rule then skips the unit).
  Uid uid {unknown_uid};
  std::vector<int> status_cols {};
  /// Block UIDs parallel to `status_cols` — the block half of the semantic
  /// (generator, block) seed key.  Empty when no external seed is in play.
  std::vector<Uid> block_uids {};
};

/// A dense external commitment seed: `seed_commitment_key(gen_uid, block_uid)`
/// → status `u` (any value; the rule thresholds at 0.5).  Populated from an
/// external solution (previous-day PLEXOS / gtopt, nearest-historical, an ML
/// predictor, …) and consumed by `SeedCommitmentRule`.  The producer owns
/// cross-day alignment AND commitment feasibility (it emits keys in THIS
/// case's (generator, block) space, repaired by
/// `gtopt_warmstart.build_full_seed`); gtopt just matches by identity.
using SeedCommitmentMap = std::unordered_map<std::uint64_t, double>;

/// Pack a (generator uid, block uid) pair into the `SeedCommitmentMap` key.
/// Both uids are `int32`; cast through `uint32` so a negative sentinel keeps a
/// stable bit pattern rather than sign-extending across the two halves.
[[nodiscard]] constexpr std::uint64_t seed_commitment_key(
    Uid gen_uid, Uid block_uid) noexcept
{
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gen_uid))
          << 32U)
      | static_cast<std::uint64_t>(static_cast<std::uint32_t>(block_uid));
}

/// The domain data a rule reads.  EXTENSION POINT: add fields as new rules
/// need them; existing rules ignore fields they don't read, so the struct can
/// grow without touching them.
struct DomainRuleContext
{
  std::span<const CommitmentRunInfo> commitments {};
};

/// A single domain rule that adjusts a candidate integer commitment
/// (the dense, raw-column-indexed `values`, modified IN PLACE).  Rules must be
/// stateless and `const`-applied so the default pipeline can be shared across
/// threads.
class DomainRule
{
public:
  DomainRule() = default;
  virtual ~DomainRule() = default;
  DomainRule(const DomainRule&) = delete;
  DomainRule& operator=(const DomainRule&) = delete;
  DomainRule(DomainRule&&) = delete;
  DomainRule& operator=(DomainRule&&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Adjust `values` in place; @return the number of column values flipped
  /// (0 = the rule found nothing to change).
  [[nodiscard]] virtual int apply(std::span<double> values,
                                  const DomainRuleContext& ctx) const = 0;
};

/// External-commitment seed rule: overwrite each commitment unit's status (u)
/// with a value from an externally-supplied `SeedCommitmentMap`, matched by the
/// semantic (generator uid, block uid) key.  Units / blocks absent from the
/// seed are left at their rounded-relaxation value, so a partial seed (only the
/// elements that exist in both the source solution and this case) is fine — the
/// "existing elements" semantics.  A strategy (previous-day PLEXOS / gtopt,
/// nearest-historical, ML) is just a producer of the map; this rule is the
/// single generic consumer.
class SeedCommitmentRule final : public DomainRule
{
public:
  explicit SeedCommitmentRule(SeedCommitmentMap seed)
      : m_seed_(std::move(seed))
  {
  }
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "seed_commitment";
  }
  [[nodiscard]] int apply(std::span<double> values,
                          const DomainRuleContext& ctx) const override;

private:
  SeedCommitmentMap m_seed_;
};

/// An ordered set of `DomainRule`s applied as a pipeline.
class DomainRulePipeline
{
public:
  /// Append a rule (ignored if null).
  void add(std::unique_ptr<DomainRule> rule);

  /// Apply every rule in order.  When `max_passes > 1`, repeat the whole
  /// sequence until a pass flips nothing (a fixpoint) or the cap is hit.
  /// @return total column values flipped.
  [[nodiscard]] int apply(std::span<double> values,
                          const DomainRuleContext& ctx,
                          int max_passes = 1) const;

  [[nodiscard]] bool empty() const noexcept { return m_rules_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return m_rules_.size(); }

private:
  std::vector<std::unique_ptr<DomainRule>> m_rules_ {};
};

/// Build the default domain-rule pipeline: a `SeedCommitmentRule` when `seed`
/// is non-empty, otherwise an empty pipeline (the rounded candidate passes
/// through unchanged).
[[nodiscard]] DomainRulePipeline make_default_domain_rules(
    SeedCommitmentMap seed = {});

}  // namespace gtopt
