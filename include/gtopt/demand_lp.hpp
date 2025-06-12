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
  constexpr static std::string_view ClassName = "Demand";

  using CapacityBase = CapacityObjectLP<Demand>;

  [[nodiscard]]
  explicit DemandLP(const InputContext& ic, Demand pdemand);

  [[nodiscard]]
  constexpr auto&& demand() const noexcept
  {
    return object();
  }

  [[nodiscard]]
  constexpr auto bus() const noexcept
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
  constexpr auto&& load_cols_at(const ScenarioIndex scenary_index,
                                const StageIndex stage_index) const noexcept
  {
    return load_cols.at({scenary_index, stage_index});
  }

private:
  OptTBRealSched lmax;
  OptTRealSched lossfactor;
  OptTRealSched fcost;
  OptTRealSched emin;
  OptTRealSched ecost;

  STBIndexHolder load_cols;
  STBIndexHolder capacity_rows;
  GSTIndexHolder emin_cols;
  GSTIndexHolder emin_rows;
};

}  // namespace gtopt
