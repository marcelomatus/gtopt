/**
 * @file      domain_rules.cpp
 * @brief     Electric-system commitment-repair rules + pipeline
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>

#include <gtopt/domain_rules.hpp>

namespace gtopt
{

int MinUpDownRule::apply(std::span<double> values,
                         const DomainRuleContext& ctx) const
{
  constexpr double eps = 1e-9;
  int flipped = 0;
  for (const auto& c : ctx.commitments) {
    const std::size_t n = c.status_cols.size();
    if (n < 2 || (c.min_up_hours <= 0.0 && c.min_down_hours <= 0.0)) {
      continue;
    }
    const auto val = [&](std::size_t t) -> double&
    { return values[static_cast<std::size_t>(c.status_cols[t])]; };

    int state = (val(0) >= 0.5) ? 1 : 0;
    double run = c.durations[0];
    for (std::size_t t = 1; t < n; ++t) {
      const int u = (val(t) >= 0.5) ? 1 : 0;
      if (u == state) {
        run += c.durations[t];
        continue;
      }
      const double min_run = (state == 1) ? c.min_up_hours : c.min_down_hours;
      if (run + eps < min_run) {
        // Premature transition — extend the current run by suppressing it.
        val(t) = static_cast<double>(state);
        run += c.durations[t];
        ++flipped;
      } else {
        state = u;
        run = c.durations[t];
      }
    }
  }
  return flipped;
}

int PeakInjectionRule::apply(std::span<double> values,
                             const DomainRuleContext& ctx) const
{
  int flipped = 0;
  for (const auto& inj : ctx.injections) {
    const std::size_t n = inj.status_cols.size();
    // Parallel arrays — guard against a malformed entry (defensive; the hook
    // always builds them in lockstep).
    const std::size_t m = std::min(n, inj.is_peak.size());
    for (std::size_t t = 0; t < m; ++t) {
      if (inj.is_peak[t] == 0) {
        continue;  // off-peak block — leave the rounded value untouched
      }
      auto& v = values[static_cast<std::size_t>(inj.status_cols[t])];
      // Conservative: only nudge an idle/ambiguous unit ON; never turn a
      // committed (>= 0.5) unit OFF, so a decisive LP-relaxation signal stands.
      if (v < 0.5) {
        v = 1.0;
        ++flipped;
      }
    }
  }
  return flipped;
}

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

DomainRulePipeline make_default_domain_rules()
{
  DomainRulePipeline pipeline;
  pipeline.add(std::make_unique<MinUpDownRule>());
  // Peak-injection bias runs AFTER run-length repair: it only forces u ON at
  // peak (never OFF), so it cannot create a sub-min-up run; a following pass of
  // MinUpDownRule (max_passes > 1) would absorb any min-down it shortens.
  pipeline.add(std::make_unique<PeakInjectionRule>());
  // Future electric-system rules register here — each one encodes more
  // power-system knowledge into the initial integer guess, e.g.:
  //   pipeline.add(std::make_unique<RampFeasibilityRule>());
  //   pipeline.add(std::make_unique<MustRunRule>());
  //   pipeline.add(std::make_unique<ReserveCoverageRule>());
  return pipeline;
}

}  // namespace gtopt
