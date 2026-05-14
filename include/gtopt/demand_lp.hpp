#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/demand.hpp>

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

  [[nodiscard]]
  constexpr const auto& fail_cols_at(const ScenarioLP& scenario,
                                     const StageLP& stage) const
  {
    return fail_cols.at({scenario.uid(), stage.uid()});
  }

  /// Return the demand-failure slack column for (scenario, stage, block),
  /// if it exists.  The slack is only created when `fail_cost > 0` on the
  /// containing stage and the demand is not forced; returns `std::nullopt`
  /// otherwise.  Used by SystemLP::accumulate_convergence_indicators().
  [[nodiscard]]
  constexpr std::optional<ColIndex> fail_col_at(
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BlockLP& block) const noexcept
  {
    const auto st_it = fail_cols.find({scenario.uid(), stage.uid()});
    if (st_it == fail_cols.end()) {
      return std::nullopt;
    }
    const auto& by_block = st_it->second;
    const auto b_it = by_block.find(block.uid());
    if (b_it == by_block.end()) {
      return std::nullopt;
    }
    return b_it->second;
  }

  /// @name Parameter accessors for user constraint resolution
  /// @{
  [[nodiscard]] auto param_lmax(StageUid s, BlockUid b) const
  {
    return lmax.at(s, b);
  }
  [[nodiscard]] auto param_fcost(StageUid s) const { return fcost.at(s); }
  [[nodiscard]] auto param_lossfactor(StageUid s) const
  {
    return lossfactor.at(s);
  }
  /// @}

private:
  OptTBRealSched lmax;
  OptTRealSched lossfactor;
  OptTRealSched fcost;

  OptTRealSched emin;
  OptTRealSched ecost;

  STBIndexHolder<ColIndex> load_cols;
  STBIndexHolder<RowIndex> capacity_rows;
  STIndexHolder<ColIndex> emin_cols;
  STIndexHolder<RowIndex> emin_rows;

  STBIndexHolder<ColIndex> lman_cols;

  STBIndexHolder<ColIndex> fail_cols;
  STBIndexHolder<RowIndex> balance_rows;
};

using DemandLPId = ObjectId<DemandLP>;
using DemandLPSId = ObjectSingleId<DemandLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Demand::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Demand"`).
static_assert(DemandLP::Element::class_name == LPClassName {"Demand"},
              "Demand::class_name must remain \"Demand\"");

}  // namespace gtopt
