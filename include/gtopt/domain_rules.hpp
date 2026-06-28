/**
 * @file      domain_rules.hpp
 * @brief     Electric-system rules that repair a candidate integer commitment
 * @date      2026-06-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The MIP-start pipeline has three cleanly separated concerns:
 *
 *   1. **Rounding** — generic, domain-agnostic: round each relaxed integer
 *      column to {0,1} by a threshold (`detail::round_with_threshold`).  Any
 *      solver / any problem can do this; it knows nothing about power systems.
 *   2. **Electric-system repair** — THIS file: a pluggable, ordered set of
 *      `DomainRule`s that adjust the rounded commitment using
 *      power-system knowledge the rounding cannot (today: minimum on/off run
 *      lengths; future: ramp feasibility, must-run/-off, reserve coverage, …).
 *   3. **Solver refinement** — per-generator: re-solve a fixed-commitment ED-LP
 *      (`relax_fix`) or hand the repaired commitment to SCIP (`scip_repair`).
 *
 * Adding a new electric-system rule that produces a better initial integer
 * guess is a LOCAL change: subclass `DomainRule`, then register it in
 * `make_default_domain_rules()`.  Every generator (`lp_round`,
 * `relax_fix`, `scip_repair`) picks it up automatically — they all apply the
 * same pipeline between rounding and solver refinement.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace gtopt
{

/// One commitment unit's status (u) columns in chronological order, with the
/// matching block durations and the unit's min-up/min-down times (hours).  The
/// domain input a run-length rule needs — built by the `SystemLP::resolve` hook
/// (one entry per (scenario, commitment) with a run-length limit).
/// `status_cols[t]` is a RAW column index; `durations[t]` is the block duration
/// [h] of that period (parallel arrays).
struct CommitmentRunInfo
{
  double min_up_hours {0.0};
  double min_down_hours {0.0};
  std::vector<int> status_cols {};
  std::vector<double> durations {};
};

/// One storage / hydro injection unit's status (u) columns in chronological
/// order, with a parallel flag marking which blocks the rule should seed ON.
/// The domain input the `PeakInjectionRule` needs — built by the
/// `SystemLP::resolve` hook (one entry per storage-injection commitment: the
/// `u` of a reservoir-fed hydro generator or a battery-discharge converter,
/// both reachable through `CommitmentLP::find_status_cols`).  The hook picks
/// the seed window PER UNIT CLASS: hydro → the evening peak window; battery →
/// the solar charge window plus the evening discharge window (one daily cycle).
/// Only chronological stages carry commitment columns, so `status_cols` is
/// naturally restricted to where a wall-clock hour-of-day is meaningful.
/// `status_cols[t]` is a RAW column index; `is_peak[t] != 0` marks block t as a
/// seed-ON block (parallel arrays).
struct PeakInjectionInfo
{
  std::vector<int> status_cols {};
  std::vector<char> is_peak {};
};

/// The electric-system data a repair rule reads.  EXTENSION POINT: add fields
/// (reserve requirements, ramp limits, demand profile, hydro coupling, …) as
/// new rules need them; existing rules ignore fields they don't read, so the
/// struct can grow without touching them.
struct DomainRuleContext
{
  std::span<const CommitmentRunInfo> commitments {};
  std::span<const PeakInjectionInfo> injections {};
};

/// A single electric-system rule that adjusts a candidate integer commitment
/// (the dense, raw-column-indexed `values`, modified IN PLACE) toward
/// feasibility using power-system knowledge.  Rules must be stateless and
/// `const`-applied so the default pipeline can be shared across threads.
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
  /// (0 = the rule found nothing to fix).
  [[nodiscard]] virtual int apply(std::span<double> values,
                                  const DomainRuleContext& ctx) const = 0;
};

/// Min-up / min-down run-length rule: a greedy forward sweep per unit that
/// suppresses any transition which would end a run shorter than the unit's
/// minimum on/off time, by extending the current run.  Makes the rounded
/// commitment respect the run-length rules so a fixed-commitment dispatch LP is
/// feasible and the start passes CHECKFEAS.
class MinUpDownRule final : public DomainRule
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "min_up_down";
  }
  [[nodiscard]] int apply(std::span<double> values,
                          const DomainRuleContext& ctx) const override;
};

/// Peak-injection bias rule: seed storage / hydro injection units to inject
/// (discharge / generate) during peak hours.  For each `PeakInjectionInfo`
/// unit, on every block flagged `is_peak`, set the unit's status (u) column ON
/// (1.0) when the rounded candidate has it OFF/ambiguous (< 0.5).
///
/// CONSERVATIVE POLICY — this rule only ever flips a status binary toward ON,
/// and only at peak blocks: it never turns a committed unit OFF and never
/// touches off-peak blocks.  So it cannot override a decisive LP-relaxation
/// signal to keep a unit committed; it only nudges idle-but-available storage
/// to be online when the system is most stressed.  The MIP then validates and
/// re-optimizes the seed, so a wrong nudge costs at most a little
/// branch-and-cut work, never correctness.  Empty `ctx.injections` ⇒ no-op (the
/// feature is gated by the `SystemLP::resolve` hook only populating it when
/// `mip_start.peak_injection` is enabled), exactly like `MinUpDownRule` skips
/// units without run-length limits.
class PeakInjectionRule final : public DomainRule
{
public:
  [[nodiscard]] std::string_view name() const noexcept override
  {
    return "peak_injection";
  }
  [[nodiscard]] int apply(std::span<double> values,
                          const DomainRuleContext& ctx) const override;
};

/// An ordered set of `DomainRule`s applied as a pipeline.
class DomainRulePipeline
{
public:
  /// Append a rule (ignored if null).
  void add(std::unique_ptr<DomainRule> rule);

  /// Apply every rule in order.  When `max_passes > 1`, repeat the whole
  /// sequence until a pass flips nothing (a fixpoint) or the cap is hit —
  /// rules can interact (mending ramp may break run-length and vice versa), so
  /// re-passing converges them.  @return total column values flipped.
  [[nodiscard]] int apply(std::span<double> values,
                          const DomainRuleContext& ctx,
                          int max_passes = 1) const;

  [[nodiscard]] bool empty() const noexcept { return m_rules_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return m_rules_.size(); }

private:
  std::vector<std::unique_ptr<DomainRule>> m_rules_ {};
};

/// Build the default electric-system repair pipeline.  THE place to register
/// future rules that encode more power-system knowledge into the initial
/// integer guess (ramp, must-run, reserve coverage, …).  Currently:
/// `MinUpDownRule`, then `PeakInjectionRule`.  Both are data-gated: a rule
/// no-ops when its slice of `DomainRuleContext` is empty, so registering by
/// default is free for runs that don't supply the data.
[[nodiscard]] DomainRulePipeline make_default_domain_rules();

}  // namespace gtopt
