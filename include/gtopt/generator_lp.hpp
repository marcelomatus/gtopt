/**
 * @file      generator_lp.hpp
 * @brief     Header of
 * @date      Sat Mar 29 00:53:51 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */
#pragma once

#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{
using GeneratorLPId = ObjectId<class GeneratorLP>;
using GeneratorLPSId = ObjectSingleId<class GeneratorLP>;

class GeneratorLP : public CapacityObjectLP<Generator>
{
public:
  constexpr static std::string_view ClassName = "Generator";

  using CapacityBase = CapacityObjectLP<Generator>;

  explicit GeneratorLP(const InputContext& ic, Generator pgenerator);

  [[nodiscard]] constexpr auto&& generator() { return object(); }
  [[nodiscard]] constexpr auto&& generator() const { return object(); }
  [[nodiscard]] auto bus() const { return BusLPSId {generator().bus}; }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioIndex& scenario_index,
                               const StageIndex& stage_index,
                               LinearProblem& lp);
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& generation_cols_at(const ScenarioIndex scenary_index,
                                          const StageIndex stage_index) const
  {
    return generation_cols.at({scenary_index, stage_index});
  }

private:
  OptTBRealSched pmin;
  OptTBRealSched pmax;
  OptTRealSched lossfactor;
  OptTRealSched gcost;

  STBIndexHolder generation_cols;
  STBIndexHolder capacity_rows;

  ElementIndex<BusLP> bus_index;
};

}  // namespace gtopt
