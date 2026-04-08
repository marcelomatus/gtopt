/**
 * @file      commitment_lp.hpp
 * @brief     LP formulation for unit commitment (three-bin u/v/w model)
 * @date      Tue Apr  8 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines CommitmentLP which creates binary (or relaxed) status/startup/
 * shutdown variables for a generator and adds the three core UC constraints:
 * - C1 (logic): u[t] − u[t−1] = v[t] − w[t]
 * - C2 (generation limits): Pmin·u ≤ p ≤ Pmax·u
 * - C3 (exclusion): v[t] + w[t] ≤ 1
 *
 * Also handles emission cost adder on the generation variable when the
 * generator has an emission_factor and the system defines emission_cost.
 */

#pragma once

#include <gtopt/commitment.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/schedule.hpp>

namespace gtopt
{

class CommitmentLP : public ObjectLP<Commitment>
{
public:
  static constexpr LPClassName ClassName {"Commitment", "cmt"};
  static constexpr std::string_view StatusName {"status"};
  static constexpr std::string_view StartupName {"startup"};
  static constexpr std::string_view ShutdownName {"shutdown"};
  static constexpr std::string_view GenUpperName {"gen_upper"};
  static constexpr std::string_view GenLowerName {"gen_lower"};
  static constexpr std::string_view LogicName {"logic"};
  static constexpr std::string_view ExclusionName {"exclusion"};
  static constexpr std::string_view RampUpName {"ramp_up"};
  static constexpr std::string_view RampDownName {"ramp_down"};
  static constexpr std::string_view SegmentName {"segment"};
  static constexpr std::string_view MinUpTimeName {"min_up_time"};
  static constexpr std::string_view MinDownTimeName {"min_down_time"};

  using Base = ObjectLP<Commitment>;

  explicit CommitmentLP(const Commitment& commitment, const InputContext& ic);

  [[nodiscard]] constexpr auto&& commitment(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {commitment().generator};
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  ElementIndex<GeneratorLP> generator_index_;
  OptTRealSched startup_cost_;
  OptTRealSched shutdown_cost_;
  OptTRealSched fuel_cost_;

  STBIndexHolder<ColIndex> status_cols_;
  STBIndexHolder<ColIndex> startup_cols_;
  STBIndexHolder<ColIndex> shutdown_cols_;
  STBIndexHolder<RowIndex> logic_rows_;
  STBIndexHolder<RowIndex> gen_upper_rows_;
  STBIndexHolder<RowIndex> gen_lower_rows_;
  STBIndexHolder<RowIndex> exclusion_rows_;
  STBIndexHolder<RowIndex> ramp_up_rows_;
  STBIndexHolder<RowIndex> ramp_down_rows_;

  /// Per-segment generation columns (outer key = segment index as int)
  /// Each entry maps (scenario, stage) → block → ColIndex
  std::vector<STBIndexHolder<ColIndex>> segment_cols_;
  /// Linking rows: p - Pmin·u - Σ δ_k = 0
  STBIndexHolder<RowIndex> segment_link_rows_;
  STBIndexHolder<RowIndex> min_up_time_rows_;
  STBIndexHolder<RowIndex> min_down_time_rows_;
};

}  // namespace gtopt
