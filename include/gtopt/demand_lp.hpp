#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/demand.hpp>
#include <gtopt/linear_interface.hpp>  // ScaledView

namespace gtopt
{

class DemandLP : public CapacityObjectLP<Demand>
{
public:
  static constexpr std::string_view LoadName {"load"};
  static constexpr std::string_view FailName {"fail"};
  static constexpr std::string_view BalanceName {"balance"};
  static constexpr std::string_view CapacityName {"capacity"};
  static constexpr std::string_view EminName {"emin"};
  static constexpr std::string_view LmanName {"lman"};
  /// Filter metadata keys published by `add_to_lp` for `sum(...)`
  /// predicate matching.
  static constexpr std::string_view TypeKey {"type"};
  static constexpr std::string_view BusKey {"bus"};

  using CapacityBase = CapacityObjectLP<Demand>;

  [[nodiscard]]
  explicit DemandLP(const Demand& pdemand, const InputContext& ic);

  [[nodiscard]] constexpr auto&& demand(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]]
  constexpr auto bus_sid() const noexcept
  {
    return ObjectSingleId<BusLP> {demand().bus};
  }

  [[nodiscard]]
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  [[nodiscard]]
  bool add_to_output(OutputContext& out) const;

  [[nodiscard]]
  constexpr const auto& load_cols_at(const ScenarioLP& scenario,
                                     const StageLP& stage) const
  {
    return load_cols.at({scenario.uid(), stage.uid()});
  }

  /// Reconstructed failure quantity at (scenario, stage, block).
  ///
  /// After the P0 demand-failure substitution (`fail = lmax − load`)
  /// the `fail` LP variable no longer exists.  The value is derived
  /// at read time from the surviving `load_cols` primal solution and
  /// the cached `block_lmax_values_` (post-capacity-clamp `lmax`).
  /// Returns `0.0` when no `load_cols` entry exists for the (s, t, b)
  /// — semantically "no failure because there is no demand to serve
  /// here".  `col_sol` is the LP's primal-solution view (the same
  /// span that `OutputContext::col_sol_span` wraps); callers pass it
  /// in to avoid per-call re-fetch.  Used by
  /// `SystemLP::accumulate_convergence_indicators` to track
  /// `unserved_demand`.
  [[nodiscard]] double fail_sol_at(const ScenarioLP& scenario,
                                   const StageLP& stage,
                                   const BlockLP& block,
                                   const ScaledView& col_sol) const noexcept;

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_lmax(StageUid s, BlockUid b) const
  {
    return lmax.at(s, b);
  }
  [[nodiscard]] auto param_fcost(StageUid s, BlockUid b) const
  {
    return fcost.at(s, b);
  }
  [[nodiscard]] auto param_lossfactor(StageUid s, BlockUid b) const
  {
    return lossfactor.at(s, b);
  }
  /// @}

private:
  OptTBRealSched lmax;
  OptTBRealSched lmin;
  OptTBRealSched lossfactor;
  OptTBRealSched fcost;

  OptTRealSched emin;
  OptTRealSched ecost;

  STBIndexHolder<ColIndex> load_cols;
  STBIndexHolder<RowIndex> capacity_rows;

  /// Stage-level emin-balance constraint row index (one per
  /// (scenario, stage) when emin is active).  Post-collapse this
  /// is `sum_b bdur*mcol ≤ stage_emin` (soft, ecost set) or
  /// `sum_b bdur*mcol = stage_emin` (hard, no ecost) — direct on
  /// the surviving `mcol` (`lman`) per-block columns; the
  /// pre-collapse stage-aggregator `emin_col` is gone.  Kept for
  /// potential dual-value diagnostics.
  STIndexHolder<RowIndex> emin_rows;

  STBIndexHolder<ColIndex> lman_cols;

  /// Cached `block_lmax` post-capacity-clamp values populated during
  /// `add_to_lp` and consumed by `fail_sol_at` / `add_to_output` to
  /// reconstruct `Demand/fail_sol.csv` after the P0 demand-failure
  /// substitution removed the `fail` LP column.  One entry per
  /// (scenario, stage, block) that participates in the load path.
  STBIndexHolder<double> block_lmax_values_;

  /// Cached AMPL offset values for Option C blocks.  Only populated
  /// for (scenario, stage, block) cells where the column represents
  /// `neg_fail = load − lmax` (i.e. `fail_substituted == true` in
  /// `add_to_lp`).  Used by `add_to_output` to distinguish Option C
  /// reconstruction (`load = col_primal + offset`,
  /// `fail = −col_primal`) from the forced/non-substituted path where
  /// the column already represents `load` directly.
  STBIndexHolder<double> block_offset_values_;

  /// True once any (scenario, stage, block) cell carries a strictly
  /// positive failure cost (effective `fcost` after the global
  /// `demand_fail_cost` fallback).  Set in `add_to_lp`.  When it stays
  /// false the demand's failure is unpenalized — e.g. a battery-charge
  /// `<name>_dem` pinned to `fcost == 0` by `System::expand_batteries`:
  /// its reconstructed `fail = lmax − load` is just unused dispatchable
  /// load capacity, not curtailment, so `add_to_output` skips emitting
  /// `Demand/fail_sol` for it (avoids e.g. 211 TWh of battery
  /// non-charging polluting the failure output).
  bool fail_penalized_ {false};
};

using DemandLPId = ObjectId<DemandLP>;
using DemandLPSId = ObjectSingleId<DemandLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Demand::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Demand"`).
static_assert(DemandLP::Element::class_name == LPClassName {"Demand"},
              "Demand::class_name must remain \"Demand\"");

}  // namespace gtopt
