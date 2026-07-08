/**
 * @file      flow_right_lp.hpp
 * @brief     LP representation of flow-based water rights
 * @date      Tue Apr  1 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Each FlowRight installs a single LP column `flow_b` per (scenario,
 * stage, block) bounded by the hard physical band `[fmin_b, fmax_b]`.
 *
 * When the `target` schedule is set, the kink at `target` is expressed
 * via two non-negative slacks (`excess` = sp, `fail` = sn) and one
 * equality row (`flow_kink`):
 *
 *     flow_b − excess_b + fail_b = target_b
 *     excess_b ≥ 0,  cost(excess_b) = -uvalue·cf  (bonus above target)
 *     fail_b   ≥ 0,  cost(fail_b)   = +fcost·cf   (penalty below target)
 *
 * When `use_average` is true, the same kink machinery is applied at
 * stage scope to a single `qeh` column instead of per-block — see
 * `attach_kink` in `flow_right_lp.cpp`.  Per-block flow columns then
 * carry only their `[fmin_b, fmax_b]` bounds; all target / cost logic
 * lives at the stage layer.
 *
 * The flow right is NOT part of the hydrological topology by itself:
 * it creates its own columns and constraints for rights accounting and
 * subtracts the per-block flow from the upstream Junction balance when
 * the `junction` reference is set.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/flow_right.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/right_bound_rule.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/update_context.hpp>

namespace gtopt
{

// Forward declaration to avoid circular includes
class SystemLP;
class SimulationLP;

using FlowRightLPSId = ObjectSingleId<class FlowRightLP>;

class FlowRightLP : public ObjectLP<FlowRight>
{
public:
  /// Per-block primary flow column (`flow_b ∈ [fmin_b, fmax_b]`).
  static constexpr std::string_view FlowName {"flow"};
  /// Per-block deficit slack (`fail_b = max(0, target_b − flow_b)`).
  /// Cost `+fcost·cf` so the LP pays a penalty when undershooting target.
  static constexpr std::string_view FailName {"fail"};
  /// Per-block surplus slack (`excess_b = max(0, flow_b − target_b)`).
  /// Cost `-uvalue·cf` so the LP earns a bonus when overshooting target.
  static constexpr std::string_view ExcessName {"excess"};
  /// Per-block kink equality row name
  /// (`flow_b − excess_b + fail_b = target_b`).
  static constexpr std::string_view FlowKinkName {"flow_kink"};

  /// Stage-average hourly flow column (`qeh ∈ [fmin_e, fmax_e]`).
  /// Only emitted when `use_average = true`.
  static constexpr std::string_view QehName {"qeh"};
  /// Stage-level surplus slack (`qeh_sp = max(0, qeh − target_e)`).
  /// Cost `-uvalue_e·cf` (excess bonus).
  static constexpr std::string_view QehSpName {"qeh_sp"};
  /// Stage-level deficit slack (`qeh_sn = max(0, target_e − qeh)`).
  /// Cost `+fcost_e·cf` (deficit penalty).
  static constexpr std::string_view QehSnName {"qeh_sn"};
  /// Stage-aggregation row name (`qeh − Σ_b dur_ratio_b × flow_b = 0`).
  static constexpr std::string_view QavgName {"qavg"};
  /// Stage-level kink equality row name
  /// (`qeh − qeh_sp + qeh_sn = target_e`).
  static constexpr std::string_view QkinkName {"qkink"};

  static constexpr std::string_view DemandName {"demand"};

  /// Per-block pass-through bypass column (`bypass_b ≥ 0`).  Emitted
  /// only when ``FlowRight::junction_b`` is set: contributes
  /// negatively to ``junction_a``'s balance row (water leaves the
  /// source junction) and positively to ``junction_b``'s
  /// balance row (water arrives downstream).  Priced at
  /// ``bypass_cost · cf`` so the LP only uses it when pressure
  /// relief is required.
  static constexpr std::string_view BypassName {"bypass"};

  explicit FlowRightLP(const FlowRight& pflow, const InputContext& ic);

  [[nodiscard]] constexpr auto&& flow_right(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Update volume-dependent column bounds when `bound_rule` is set.
  /// Reads the referenced reservoir's current volume (or the stage
  /// month for stage-month axes), evaluates the piecewise-linear bound
  /// function, and clamps `flow_b`'s `[fmin_b, fmax_b]` bounds (and the
  /// stage `qeh` bounds, when `use_average = true`).
  /// @return Number of LP column bounds modified (0 if unchanged)
  [[nodiscard]] int update_lp(SystemLP& sys,
                              const ScenarioLP& scenario,
                              const StageLP& stage);

  [[nodiscard]] auto&& flow_cols_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return flow_cols.at({scenario.uid(), stage.uid()});
  }

  /// Deficit primal at (scenario, stage, block).  Reads `fail_b`
  /// directly when the kink slacks are present; returns 0.0 otherwise.
  /// `col_sol` is the LP's primal-solution view (the same span that
  /// `OutputContext::col_sol_span` wraps).  Used by
  /// `SystemLP::accumulate_convergence_indicators` to track
  /// `unserved_flow`.
  [[nodiscard]] double fail_sol_at(const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   const BlockLP& block,
                                   const ScaledView& col_sol) const noexcept;

  /// Surplus primal at (scenario, stage, block).  Reads `excess_b`
  /// directly when the kink slacks are present; returns 0.0 otherwise.
  [[nodiscard]] double excess_sol_at(const ScenarioLP& scenario,
                                     const StageLP& stage,
                                     const BlockLP& block,
                                     const ScaledView& col_sol) const noexcept;

  /// Return the stage-average hourly flow column for (scenario, stage).
  /// Only present when `use_average = true`.
  /// @throws std::out_of_range if not available.
  [[nodiscard]] ColIndex qeh_col_at(const ScenarioLP& scenario,
                                    const StageLP& stage) const
  {
    return qeh_cols.at({scenario.uid(), stage.uid()});
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_fmin(StageUid s, BlockUid b) const
  {
    return fmin_sched.at(s, b);
  }
  [[nodiscard]] auto param_fmax(StageUid s, BlockUid b) const
  {
    return fmax_sched.at(s, b);
  }
  [[nodiscard]] auto param_target(StageUid s, BlockUid b) const
  {
    return target_sched.at(s, b);
  }
  [[nodiscard]] auto param_fcost(StageUid s, BlockUid b) const
  {
    return fcost.at(s, b);
  }
  [[nodiscard]] auto param_uvalue(StageUid s, BlockUid b) const
  {
    return uvalue_sched.at(s, b);
  }

  /// @brief Test-only existence probes for LP entities.
  ///
  /// These return *only* whether the corresponding LP entity was
  /// installed by `add_to_lp`; they do not expose the indices.  The LP
  /// shape across the three FlowMode values (`per_block`,
  /// `stage_average`, `stage_uniform`) differs in which slack/row maps
  /// are populated, and existence checks are the cleanest way for tests
  /// to pin the shape independently of solver primals.  Each probe is a
  /// pure container query (`map::find` or `map::contains`), so it can
  /// be called on any built `FlowRightLP` regardless of whether the LP
  /// has been solved.
  [[nodiscard]] bool has_qeh(const ScenarioLP& scenario,
                             const StageLP& stage) const noexcept
  {
    return qeh_cols.contains({scenario.uid(), stage.uid()});
  }
  [[nodiscard]] bool has_qeh_slacks(const ScenarioLP& scenario,
                                    const StageLP& stage) const noexcept
  {
    const auto key = std::tuple {scenario.uid(), stage.uid()};
    return qeh_sp_cols.contains(key) || qeh_sn_cols.contains(key);
  }
  [[nodiscard]] bool has_qkink_row(const ScenarioLP& scenario,
                                   const StageLP& stage) const noexcept
  {
    return qkink_rows.contains({scenario.uid(), stage.uid()});
  }
  [[nodiscard]] bool has_qavg_row(const ScenarioLP& scenario,
                                  const StageLP& stage) const noexcept
  {
    return avg_rows.contains({scenario.uid(), stage.uid()});
  }
  [[nodiscard]] bool has_block_slacks(const ScenarioLP& scenario,
                                      const StageLP& stage) const noexcept
  {
    const auto key = std::tuple {scenario.uid(), stage.uid()};
    return fail_cols.contains(key) || excess_cols.contains(key);
  }

  /// @brief Test-only column-index lookup for the per-block deficit
  /// slack.  Returns `std::nullopt` when no slack was installed at the
  /// given (scenario, stage, block) — i.e. when the per-block kink
  /// machinery did not fire (no target, or both `fcost` and `uvalue`
  /// inactive, or `flow_mode != per_block`).  Used by tests to inspect
  /// the slack's objective coefficient via `lp.get_obj_coeff()[col]`.
  [[nodiscard]] std::optional<ColIndex> fail_col_at(
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BlockLP& block) const noexcept
  {
    const auto st_it = fail_cols.find({scenario.uid(), stage.uid()});
    if (st_it == fail_cols.end()) {
      return std::nullopt;
    }
    const auto b_it = st_it->second.find(block.uid());
    if (b_it == st_it->second.end()) {
      return std::nullopt;
    }
    return b_it->second;
  }
  /// Test-only sibling of `fail_col_at` for the excess (sp) slack.
  [[nodiscard]] std::optional<ColIndex> excess_col_at(
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BlockLP& block) const noexcept
  {
    const auto st_it = excess_cols.find({scenario.uid(), stage.uid()});
    if (st_it == excess_cols.end()) {
      return std::nullopt;
    }
    const auto b_it = st_it->second.find(block.uid());
    if (b_it == st_it->second.end()) {
      return std::nullopt;
    }
    return b_it->second;
  }
  /// @}

private:
  /// Resolved fmin schedule (hard lower bound).
  /// Per-stage-block so hourly profiles (Demand::lmax-style) work; the
  /// LP build performs an `.at(stage_uid, block_uid)` lookup per block.
  OptTBRealSched fmin_sched;
  /// Resolved fmax schedule (hard upper bound).  Per-stage-block.
  OptTBRealSched fmax_sched;
  /// Resolved target schedule (soft kink, aliases legacy `discharge`).
  /// Per-stage-block so hourly forced-flow profiles round-trip through
  /// parquet without "duplicate uid" warnings on the C++ reader.
  OptTBRealSched target_sched;
  int direction {-1};

  /// Resolved fcost schedule for per-(stage, block) deficit penalty cost.
  /// Mirrors Demand::fcost structure (per-(stage, block) since PR-C);
  /// the LP cost coefficient at each block is built via
  /// `CostHelper::block_ecost(scenario, stage, block, block_fcost)`.
  OptTBRealSched fcost;
  /// Resolved uvalue schedule for per-(stage, block) excess bonus.
  OptTBRealSched uvalue_sched;

  /// Per-block primary flow column `flow_b ∈ [fmin_b, fmax_b]`.
  STBIndexHolder<ColIndex> flow_cols;
  /// Per-block deficit slack `fail_b ≥ 0`.  Populated only when the
  /// per-block kink machinery fires (target resolved AND `use_average`
  /// is false).
  STBIndexHolder<ColIndex> fail_cols;
  /// Per-block surplus slack `excess_b ≥ 0`.  Same population rule as
  /// `fail_cols`.
  STBIndexHolder<ColIndex> excess_cols;
  /// Per-block kink rows `flow_b − excess_b + fail_b = target_b`.
  STBIndexHolder<RowIndex> flow_kink_rows;

  /// Stage-average flow column `qeh ∈ [fmin_e, fmax_e]` (one per scene
  /// × stage).  Only populated when `use_average = true`.
  STIndexHolder<ColIndex> qeh_cols;
  /// Stage-aggregation rows `qeh − Σ_b dur_ratio_b × flow_b = 0`.
  STIndexHolder<RowIndex> avg_rows;
  /// Stage-level surplus slack `qeh_sp ≥ 0`.  Populated when
  /// `use_average = true` AND a target is resolved on the stage.
  STIndexHolder<ColIndex> qeh_sp_cols;
  /// Stage-level deficit slack `qeh_sn ≥ 0`.  Same population rule.
  STIndexHolder<ColIndex> qeh_sn_cols;
  /// Stage-level kink rows `qeh − qeh_sp + qeh_sn = target_e`.
  STIndexHolder<RowIndex> qkink_rows;

  /// Cached resolved `target_b` per (scenario, stage, block) for the
  /// **one-sided-kink-substituted** blocks (fcost-only or uvalue-only)
  /// where the explicit fail / excess slack column and the kink row
  /// were folded into the primary flow column.  Used by
  /// `fail_sol_at` / `excess_sol_at` and `add_to_output` to
  /// reconstruct the deficit / surplus quantity from the flow primal
  /// without re-walking the schedule.  Empty for full-kink blocks
  /// (both costs active — explicit fail_cols/excess_cols are
  /// populated) and for blocks with no target.
  STBIndexHolder<double> block_target_values_;

  /// Per-block sign bit recording which side of the elided kink was
  /// active: `1` ⇒ fcost-only (fail reconstruction),
  /// `0` ⇒ uvalue-only (excess reconstruction).  Stored as
  /// `std::uint8_t` rather than `bool` because flat_map's
  /// reference proxy chokes on `bool`-valued maps.
  STBIndexHolder<std::uint8_t> block_fcost_only_;

  /// Stage-scope (qeh) one-sided-substitution cache — mirrors the
  /// per-block pair above.  Needed by ``update_lp``: a bound-rule
  /// re-clamp must preserve the substitution's target-side clamp on
  /// the qeh column (uppb = min(fmax_e, target) under fcost-only /
  /// lowb = max(fmin_e, target) under uvalue-only), otherwise the
  /// folded slack cost applies beyond the kink and corrupts the
  /// objective.
  STIndexHolder<double> qeh_target_values_;
  STIndexHolder<std::uint8_t> qeh_fcost_only_;

  /// Cached bound rule evaluation per (scenario, stage).
  /// Only populated when flow_right().bound_rule is set.
  using BoundState = RuleBoundState;
  IndexHolder2<ScenarioUid, StageUid, BoundState> m_bound_states_;
};

// Pin the data-struct constant value so an accidental rename of the
// `FlowRight::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"FlowRight"`).
static_assert(FlowRightLP::Element::class_name == LPClassName {"FlowRight"},
              "FlowRight::class_name must remain \"FlowRight\"");

}  // namespace gtopt
