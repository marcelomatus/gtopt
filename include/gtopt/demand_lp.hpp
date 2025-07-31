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
  explicit DemandLP(Demand pdemand, const InputContext& ic);

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
  constexpr auto&& load_cols_at(const ScenarioLP& scenario,
                                const StageLP& stage) const noexcept
  {
    return load_cols.at({scenario.uid(), stage.uid()});
  }

private:
  OptTBRealSched lmax;
  OptTRealSched lossfactor;
  OptTRealSched fcost;
  OptTRealSched emin;
  OptTRealSched ecost;

  STBIndexHolder<ColIndex> load_cols;
  STBIndexHolder<RowIndex> capacity_rows;
  GSTIndexHolder<ColIndex> emin_cols;
  GSTIndexHolder<RowIndex> emin_rows;
};

}  // namespace gtopt
