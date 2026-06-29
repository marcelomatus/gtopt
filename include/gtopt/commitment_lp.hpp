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
 * generator has an emission_rate and the system defines emission_cost.
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
  static constexpr std::string_view HotStartName {"hot_start"};
  static constexpr std::string_view WarmStartName {"warm_start"};
  static constexpr std::string_view ColdStartName {"cold_start"};
  static constexpr std::string_view StartupTypeName {"startup_type"};
  static constexpr std::string_view MaxStartsName {"max_starts"};

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

  /// Tolerant lookup of the per-block status (u) columns for a given
  /// (scenario, stage).  Returns nullptr when the commitment is
  /// inactive for that cell or has no status columns yet (e.g.,
  /// add_to_lp not yet called on this scenario/stage).  Used by
  /// ``ReserveProvisionLP::add_to_lp`` to inject the
  /// ``provision_col - urmin × u_commit ≥ 0`` linkage row that
  /// gates PLEXOS-style ``Min Provision`` by the commit binary.
  [[nodiscard]] const BIndexHolder<ColIndex>* find_status_cols(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    const auto it = status_cols_.find({scenario.uid(), stage.uid()});
    return it != status_cols_.end() ? &it->second : nullptr;
  }

  /// Per-block startup (v) columns for a (scenario, stage); nullptr when the
  /// commitment is inactive there.  Used by the MIP-start `CommitmentLogicRule`
  /// to derive v from the repaired status (u) transitions so the C1 logic rows
  /// (`u[p] - u[p-1] - v[p] + w[p] = 0`) hold for the injected integer start.
  [[nodiscard]] const BIndexHolder<ColIndex>* find_startup_cols(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    const auto it = startup_cols_.find({scenario.uid(), stage.uid()});
    return it != startup_cols_.end() ? &it->second : nullptr;
  }

  /// Per-block shutdown (w) columns for a (scenario, stage); nullptr when the
  /// commitment is inactive there.  Companion to `find_startup_cols`.
  [[nodiscard]] const BIndexHolder<ColIndex>* find_shutdown_cols(
      const ScenarioLP& scenario, const StageLP& stage) const
  {
    const auto it = shutdown_cols_.find({scenario.uid(), stage.uid()});
    return it != shutdown_cols_.end() ? &it->second : nullptr;
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  ElementIndex<GeneratorLP> generator_index_;
  /// Per-(stage, block) startup / shutdown costs applied to the v / w
  /// columns in the objective.  Back-compat: a scalar or per-stage JSON
  /// value broadcasts to every block, reproducing the legacy per-stage
  /// cost byte-for-byte.  Resolved block-by-block in ``add_to_lp``.
  OptTBRealSched startup_cost_;
  OptTBRealSched shutdown_cost_;
  /// Per-(stage, block) forced commitment schedule.  When set at a
  /// given block, the u column's bounds are pinned to that value
  /// (interpreted as 0 = off, 1 = on); blocks where the schedule has
  /// no entry leave u free (or governed by ``must_run``).
  OptTBRealSched fixed_status_;
  /// Per-(stage, block) minimum stable level when committed [MW]
  /// (PLEXOS ``Min Stable Level`` time series).  Resolved block-by-
  /// block; unset blocks fall back to the gen column lower bound.
  OptTBRealSched pmin_;
  /// Per-(stage, block) ramp limits / startup-shutdown output envelopes,
  /// resolved block-by-block exactly like ``pmin_``.  A scalar JSON input
  /// resolves to the same constant on every block (back-compat with the
  /// legacy scalar ``OptReal`` fields); unset blocks fall back to
  /// ``gen_pmax`` ("no limit") in the ramp rows.
  OptTBRealSched ramp_up_;  ///< Ramp-up limit [MW/hr] while online
  OptTBRealSched ramp_down_;  ///< Ramp-down limit [MW/hr] while online
  OptTBRealSched startup_ramp_;  ///< Max output in startup block [MW]
  OptTBRealSched shutdown_ramp_;  ///< Max output in shutdown block [MW]

  STBIndexHolder<ColIndex> status_cols_;
  STBIndexHolder<ColIndex> startup_cols_;
  STBIndexHolder<ColIndex> shutdown_cols_;
  STBIndexHolder<RowIndex> logic_rows_;
  STBIndexHolder<RowIndex> gen_upper_rows_;
  STBIndexHolder<RowIndex> gen_lower_rows_;
  STBIndexHolder<RowIndex> exclusion_rows_;
  STBIndexHolder<RowIndex> ramp_up_rows_;
  STBIndexHolder<RowIndex> ramp_down_rows_;

  // Piecewise-heat-rate cols and linking rows were removed from
  // CommitmentLP on 2026-05-20 — that responsibility lives in
  // GeneratorLP (Generator.pmax_segments + heat_rate_segments +
  // fuel) as a pure-LP convex-slack formulation.
  STBIndexHolder<RowIndex> min_up_time_rows_;
  STBIndexHolder<RowIndex> min_down_time_rows_;

  /// Hot/warm/cold startup type variables and constraints
  STBIndexHolder<ColIndex> hot_start_cols_;
  STBIndexHolder<ColIndex> warm_start_cols_;
  STBIndexHolder<ColIndex> cold_start_cols_;
  STBIndexHolder<RowIndex> startup_type_rows_;
  STBIndexHolder<RowIndex> hot_start_rows_;
  STBIndexHolder<RowIndex> warm_start_rows_;

  /// Per-(stage, block) ``Σ startup ≤ max_starts`` cap rows — one row
  /// per scope window (hour / day / week / horizon).  Indexed by the
  /// window-ending block uid so the dual on each row is recoverable.
  STBIndexHolder<RowIndex> max_starts_rows_;
};

// Pin the data-struct constant value so an accidental rename of the
// `Commitment::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Commitment"`).
static_assert(CommitmentLP::Element::class_name == LPClassName {"Commitment"},
              "Commitment::class_name must remain \"Commitment\"");

}  // namespace gtopt
