/**
 * @file      filtration_lp.hpp
 * @brief     Header of
 * @date      Thu Jul 31 01:49:05 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/filtration.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{

class FiltrationLP : public ObjectLP<Filtration>
{
public:
  constexpr static std::string_view ClassName = "Filtration";

  explicit FiltrationLP(Filtration pfiltration, InputContext& ic);

  [[nodiscard]] constexpr auto&& filtration(this auto&& self) noexcept
  {
    return self.object();
  }

  [[nodiscard]] constexpr auto reservoir() const noexcept
  {
    return ReservoirLPSId {filtration().reservoir};
  }
  [[nodiscard]] constexpr auto waterway() const noexcept
  {
    return WaterwayLPSId {filtration().waterway};
  }

  [[nodiscard]] constexpr auto slope() const noexcept
  {
    return filtration().slope;
  }
  [[nodiscard]] constexpr auto constant() const noexcept
  {
    return filtration().constant;
  }

  [[nodiscard]] bool add_to_lp(const SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  STBIndexHolder<ColIndex> filtration_cols;
  STBIndexHolder<RowIndex> filtration_rows;
};

}  // namespace gtopt
