#pragma once

#include <gtopt/battery.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/storage_lp.hpp>

namespace gtopt
{
using BatteryLPId = ObjectId<class BatteryLP>;
using BatteryLPSId = ObjectSingleId<class BatteryLP>;

class BatteryLP : public StorageLP<CapacityObjectLP<Battery>>
{
public:
  constexpr static std::string_view ClassName = "Battery";

  using CapacityBase = CapacityObjectLP<Battery>;
  using StorageBase = StorageLP<CapacityObjectLP<Battery>>;

  [[nodiscard]] constexpr auto&& battery() { return object(); }
  [[nodiscard]] constexpr auto&& battery() const { return object(); }

  template<typename BatteryT>
  explicit BatteryLP(const InputContext& ic, BatteryT&& pbattery)
      : StorageBase(ic, ClassName, std::forward<BatteryT>(pbattery))
  {
  }

  bool add_to_lp(const SystemContext& sc, LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& flow_cols_at(const ScenarioIndex scenary_index,
                                    const StageIndex stage_index) const
  {
    return flow_cols.at({scenary_index, stage_index});
  }

private:
  STBIndexHolder flow_cols;
};

}  // namespace gtopt
