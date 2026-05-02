/**
 * @file      simple_commitment_lp.hpp
 * @brief     LP formulation for simplified commitment
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines SimpleCommitmentLP which creates a binary status variable u
 * per block for a generator and adds two constraints:
 * - p <= Pmax * u  (upper generation limit)
 * - p >= dispatch_pmin * u  (minimum output when dispatched)
 *
 * Unlike CommitmentLP, this does not require chronological stages,
 * has no startup/shutdown variables, and no no-load cost.
 */

#pragma once

#include <gtopt/generator_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simple_commitment.hpp>

namespace gtopt
{

class SimpleCommitmentLP : public ObjectLP<SimpleCommitment>
{
public:
  static constexpr std::string_view StatusName {"status"};
  static constexpr std::string_view GenUpperName {"gen_upper"};
  static constexpr std::string_view GenLowerName {"gen_lower"};

  using Base = ObjectLP<SimpleCommitment>;

  explicit SimpleCommitmentLP(const SimpleCommitment& sc,
                              const InputContext& ic);

  [[nodiscard]] constexpr auto&& simple_commitment(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {simple_commitment().generator};
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Look up the status column for (scenario, stage, block).
  [[nodiscard]] std::optional<ColIndex> lookup_status_col(
      const ScenarioLP& scenario,
      const StageLP& stage,
      BlockUid buid) const noexcept
  {
    const auto mit = status_cols_.find({scenario.uid(), stage.uid()});
    if (mit == status_cols_.end()) {
      return std::nullopt;
    }
    const auto it = mit->second.find(buid);
    if (it == mit->second.end()) {
      return std::nullopt;
    }
    return it->second;
  }

private:
  ElementIndex<GeneratorLP> generator_index_;
  OptTBRealSched dispatch_pmin_;

  STBIndexHolder<ColIndex> status_cols_;
  STBIndexHolder<RowIndex> gen_upper_rows_;
  STBIndexHolder<RowIndex> gen_lower_rows_;
};

// Pin the data-struct constant value so an accidental rename of the
// `SimpleCommitment::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"SimpleCommitment"`).
static_assert(SimpleCommitmentLP::Element::class_name
                  == LPClassName {"SimpleCommitment"},
              "SimpleCommitment::class_name must remain \"SimpleCommitment\"");

}  // namespace gtopt
