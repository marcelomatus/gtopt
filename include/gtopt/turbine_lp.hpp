#pragma once

#include <gtopt/generator_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/turbine.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

class TurbineLP : public ObjectLP<Turbine>
{
public:
  constexpr static std::string_view ClassName = "Turbine";

  explicit TurbineLP(Turbine pturbine, InputContext& ic);

  [[nodiscard]] constexpr auto&& turbine(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto waterway() const noexcept
  {
    return WaterwayLPSId {turbine().waterway};
  }

  [[nodiscard]] constexpr auto generator() const noexcept
  {
    return GeneratorLPSId {turbine().generator};
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched conversion_rate;

  STBIndexHolder<RowIndex> conversion_rows;
};

}  // namespace gtopt
