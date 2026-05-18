/**
 * @file      converter_lp.hpp
 * @brief     LP formulation for energy converters
 * @date      Wed Apr  2 02:10:10 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the ConverterLP class, which builds LP variables and
 * constraints linking a converter to its battery, demand, and generator.
 */

#pragma once

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
  static constexpr std::string_view GenerationName {"generation"};
  static constexpr std::string_view DemandName {"demand"};
  static constexpr std::string_view CapacityName {"capacity"};
  /// PAMPL bidirectional flow compound: `flow = +discharge − charge`.
  /// Mirrors `line.flow = +flowp − flown`.  Resolves to the converter's
  /// signed net flow (positive when discharging the battery to the
  /// grid, negative when charging from the grid).  Registered as a
  /// class-level compound in `system_lp.cpp::register_all_ampl_element_names`.
  static constexpr std::string_view FlowName {"flow"};

  using CapacityBase = CapacityObjectLP<Converter>;

  [[nodiscard]] constexpr auto&& converter(this auto&& self) noexcept
  {
    return self.object();
  }

  explicit ConverterLP(const Converter& pconverter, InputContext& ic);

  [[nodiscard]] constexpr auto battery_sid() const noexcept
  {
    return BatteryLPSId {converter().battery};
  }

  [[nodiscard]] constexpr auto generator_sid() const noexcept
  {
    return GeneratorLPSId {converter().generator};
  }

  [[nodiscard]] constexpr auto demand_sid() const noexcept
  {
    return DemandLPSId {converter().demand};
  }

  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched conversion_rate;

  STBIndexHolder<RowIndex> generation_rows;
  STBIndexHolder<RowIndex> demand_rows;
  STBIndexHolder<RowIndex> capacity_rows;
};

// Pin the data-struct constant value so an accidental rename of the
// `Converter::class_name` literal fails the build (LP row labels and
// CSV outputs depend on the exact string `"Converter"`).
static_assert(ConverterLP::Element::class_name == LPClassName {"Converter"},
              "Converter::class_name must remain \"Converter\"");

}  // namespace gtopt
