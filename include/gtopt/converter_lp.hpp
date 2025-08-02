/**
 * @file      converter_lp.hpp
 * @brief     Header of
 * @date      Wed Apr  2 02:10:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <fmt/format.h>
#include <gtopt/battery_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/converter.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

class ConverterLP : public CapacityObjectLP<Converter>
{
public:
  constexpr static std::string_view ClassName = "Converter";

  using CapacityBase = CapacityObjectLP<Converter>;

  [[nodiscard]] constexpr auto&& converter(this auto&& self)
  {
    return std::forward<decltype(self)>(self).object();
  }

  explicit ConverterLP(Converter pconverter, InputContext& ic);

  [[nodiscard]] auto battery() const
  {
    return BatteryLPSId {converter().battery};
  }

  [[nodiscard]] auto generator() const
  {
    return GeneratorLPSId {converter().generator};
  }

  [[nodiscard]] auto demand() const { return DemandLPSId {converter().demand}; }

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched conversion_rate;

  STBIndexHolder<RowIndex> conversion_rows;
  STBIndexHolder<RowIndex> capacity_rows;
};

}  // namespace gtopt
