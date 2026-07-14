/**
 * @file      domain_rules.cpp
 * @brief     Domain rules: external-commitment seed application + pipeline
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>

#include <gtopt/domain_rules.hpp>

namespace gtopt
{

void DomainRulePipeline::add(std::unique_ptr<DomainRule> rule)
{
  if (rule) {
    m_rules_.push_back(std::move(rule));
  }
}

int DomainRulePipeline::apply(std::span<double> values,
                              const DomainRuleContext& ctx,
                              int max_passes) const
{
  int total = 0;
  const int passes = (max_passes < 1) ? 1 : max_passes;
  for (int p = 0; p < passes; ++p) {
    int pass_flips = 0;
    for (const auto& rule : m_rules_) {
      pass_flips += rule->apply(values, ctx);
    }
    total += pass_flips;
    if (pass_flips == 0) {
      break;  // fixpoint — no rule changed anything this pass
    }
  }
  return total;
}

int SeedCommitmentRule::apply(std::span<double> values,
                              const DomainRuleContext& ctx) const
{
  if (m_seed_.empty()) {
    return 0;
  }
  int flipped = 0;
  for (const auto& c : ctx.commitments) {
    if (c.uid == unknown_uid) {
      continue;  // no generator identity → cannot match a semantic seed
    }
    const std::size_t n = std::min(c.status_cols.size(), c.block_uids.size());
    for (std::size_t t = 0; t < n; ++t) {
      const auto it = m_seed_.find(seed_commitment_key(c.uid, c.block_uids[t]));
      if (it == m_seed_.end()) {
        continue;  // this (generator, block) is absent from the seed →
                   // keep the rounded-relaxation value ("existing elements")
      }
      const bool want_on = (it->second >= 0.5);
      auto& v = values[static_cast<std::size_t>(c.status_cols[t])];
      if ((v >= 0.5) != want_on) {
        ++flipped;
      }
      v = want_on ? 1.0 : 0.0;
    }
  }
  return flipped;
}

DomainRulePipeline make_default_domain_rules(SeedCommitmentMap seed)
{
  DomainRulePipeline pipeline;
  // The seed producer owns commitment-feasibility repair
  // (gtopt_warmstart.build_full_seed); gtopt only applies the seed by
  // semantic identity.
  if (!seed.empty()) {
    pipeline.add(std::make_unique<SeedCommitmentRule>(std::move(seed)));
  }
  return pipeline;
}

}  // namespace gtopt
