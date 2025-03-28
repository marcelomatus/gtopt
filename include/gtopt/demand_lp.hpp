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

  explicit DemandLP(const InputContext& ic, Demand&& pdemand);

  [[nodiscard]] constexpr auto&& demand() { return object(); }
  [[nodiscard]] constexpr auto&& demand() const { return object(); }
  [[nodiscard]] auto bus() const
  {
    return ObjectSingleId<BusLP> {demand().bus};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc, LinearProblem& lp);
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& load_cols_at(const SceneryIndex scenary_index,
                                    const StageIndex stage_index) const
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

  ElementIndex<BusLP> bus_index;
};

}  // namespace gtopt
