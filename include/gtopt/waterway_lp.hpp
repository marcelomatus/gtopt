/**
 * @file      waterway_lp.hpp
 * @brief     LP formulation for waterways between junctions
 * @date      Wed Jul 30 11:48:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the WaterwayLP class, which builds LP variables and
 * constraints for water flow between hydro junctions.
 */

#pragma once

#include <gtopt/junction_lp.hpp>
#include <gtopt/waterway.hpp>

namespace gtopt
{

class WaterwayLP : public ObjectLP<Waterway>
{
public:
  static constexpr std::string_view FlowName {"flow"};

  explicit WaterwayLP(const Waterway& pwaterway, const InputContext& ic);

  [[nodiscard]] constexpr auto&& waterway(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto junction_a_sid() const noexcept
  {
    return JunctionLPSId {waterway().junction_a};
  }
  [[nodiscard]] constexpr auto junction_b_sid() const noexcept
  {
    return JunctionLPSId {waterway().junction_b};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  [[nodiscard]] constexpr auto&& flow_cols_at(const ScenarioLP& scenario,
                                              const StageLP& stage) const
  {
    return flow_cols.at({scenario.uid(), stage.uid()});
  }

private:
  OptTBRealSched fmin;
  OptTBRealSched fmax;
  OptTRealSched capacity;
  OptTRealSched lossfactor;
  OptTRealSched fcost;

  STBIndexHolder<ColIndex> flow_cols;
};

using WaterwayLPId = ObjectId<class WaterwayLP>;
using WaterwayLPSId = ObjectSingleId<class WaterwayLP>;

// Pin the data-struct constant value so an accidental rename of the
// `Waterway::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Waterway"`).
static_assert(WaterwayLP::Element::class_name == LPClassName {"Waterway"},
              "Waterway::class_name must remain \"Waterway\"");

}  // namespace gtopt
