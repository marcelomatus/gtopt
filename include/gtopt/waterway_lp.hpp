/**
 * @file      waterway_lp.hpp
 * @brief     Header of
 * @date      Wed Jul 30 11:48:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/junction_lp.hpp>
#include <gtopt/waterway.hpp>

namespace gtopt
{

class WaterwayLP : public ObjectLP<Waterway>
{
public:
  constexpr static std::string_view ClassName = "Waterway";

  explicit WaterwayLP(Waterway pwaterway, const InputContext& ic);

  [[nodiscard]] constexpr const auto& waterway(this const auto& self) const noexcept
  {
    return std::forward_like<decltype(self)>(self.object());
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

  STBIndexHolder<ColIndex> flow_cols;
};

using WaterwayLPId = ObjectId<class WaterwayLP>;
using WaterwayLPSId = ObjectSingleId<class WaterwayLP>;

}  // namespace gtopt
