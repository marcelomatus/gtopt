/**
 * @file      dispatch_commitment_lp.hpp
 * @brief     LP formulation for simplified dispatch commitment
 * @date      2025
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Defines DispatchCommitmentLP which creates a binary status variable u
 * per block for a generator and adds two constraints:
 * - p ≤ Pmax · u  (upper generation limit)
 * - p ≥ dispatch_pmin · u  (minimum output when dispatched)
 *
 * Unlike CommitmentLP, this does not require chronological stages,
 * has no startup/shutdown variables, and no no-load cost.
 */

#pragma once

#include <gtopt/dispatch_commitment.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{

class DispatchCommitmentLP : public ObjectLP<DispatchCommitment>
{
public:
  static constexpr LPClassName ClassName {"DispatchCommitment"};
  static constexpr std::string_view StatusName {"status"};
  static constexpr std::string_view GenUpperName {"gen_upper"};
  static constexpr std::string_view GenLowerName {"gen_lower"};

  using Base = ObjectLP<DispatchCommitment>;

  explicit DispatchCommitmentLP(const DispatchCommitment& dc,
                                const InputContext& ic);

  [[nodiscard]] constexpr auto&& dispatch_commitment(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {dispatch_commitment().generator};
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  ElementIndex<GeneratorLP> generator_index_;
  OptTBRealSched dispatch_pmin_;

  STBIndexHolder<ColIndex> status_cols_;
  STBIndexHolder<RowIndex> gen_upper_rows_;
  STBIndexHolder<RowIndex> gen_lower_rows_;
};

}  // namespace gtopt
