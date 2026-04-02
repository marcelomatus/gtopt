#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/demand.hpp>

namespace gtopt
{

class DemandLP : public CapacityObjectLP<Demand>
{
public:
  static constexpr LPClassName ClassName {"Demand", "dem"};

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

}  // namespace gtopt
