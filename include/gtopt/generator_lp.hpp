/**
 * @file      generator_lp.hpp
 * @brief     Linear Programming representation of a Generator for optimization
 * @date      Sat Mar 29 00:53:51 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @details
 * The GeneratorLP class provides a linear programming (LP) compatible
 * representation of a Generator, which is a fundamental component for power
 * system optimization. It maintains the generator's operational constraints
 * and provides methods for LP formulation.
 *
 * @note Uses C++23 features including deducing this and structured bindings
 */

#pragma once

#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

class GeneratorLP : public CapacityObjectLP<Generator>
{
public:
  static constexpr LPClassName ClassName = {"Generator", "gen"};

  using CapacityBase = CapacityObjectLP<Generator>;

  [[nodiscard]]
  explicit GeneratorLP(Generator generator, const InputContext& ic);

  [[nodiscard]] constexpr auto&& generator(this auto&& self) noexcept
  {
    return std::forward_like<decltype(self)>(self.object());
  }

  [[nodiscard]]
  constexpr auto bus_sid() const noexcept
  {
    return BusLPSId {generator().bus};
  }

  [[nodiscard]]
  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  [[nodiscard]]
  bool add_to_output(OutputContext& out) const;

  [[nodiscard]]
  const auto& generation_cols_at(const ScenarioLP& scenario,
                                 const StageLP& stage) const
  {
    return generation_cols.at({scenario.uid(), stage.uid()});
  }

private:
  OptTBRealSched pmin;
  OptTBRealSched pmax;
  OptTRealSched lossfactor;
  OptTRealSched gcost;

  STBIndexHolder<ColIndex> generation_cols;
  STBIndexHolder<RowIndex> capacity_rows;
};

using GeneratorLPId = ObjectId<GeneratorLP>;
using GeneratorLPSId = ObjectSingleId<GeneratorLP>;

}  // namespace gtopt
