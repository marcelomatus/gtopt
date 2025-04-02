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

#include <format>

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

  [[nodiscard]] constexpr auto&& converter() { return object(); }
  [[nodiscard]] constexpr auto&& converter() const { return object(); }

  explicit ConverterLP(InputContext& ic, Converter&& pconverter);

  [[nodiscard]] auto battery() const
  {
    return BatteryLPSId {converter().battery};
  }

  [[nodiscard]] auto generator() const
  {
    if (auto&& bus_gen = converter().bus_generator) {
      if (converter().generator) {
        const auto msg = std::format(
            "in converter {} can't define both bus_generator and generator "
            "fields",
            uid());
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      return GeneratorVar {GeneratorAttrs {.bus = bus_gen.value(),
                                           .lossfactor = converter().lossfactor,
                                           .gcost = 0.0}};
    }

    if (auto&& gen = converter().generator) {
      return gen.value();
    }

    const auto msg =
        std::format("in converter {} missing bus or generator", uid());
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  [[nodiscard]] auto demand() const
  {
    if (auto&& bus_dem = converter().bus_demand) {
      if (converter().demand) {
        const auto msg = std::format(
            "in converter {} can't define both bus_demand and demand "
            "fields",
            uid());
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }
      return DemandVar {DemandAttrs {.bus = bus_dem.value(),
                                     .lossfactor = converter().lossfactor,
                                     .fcost = 0.0}};
    }

    if (auto&& dem = converter().demand) {
      return dem.value();
    }

    const auto msg =
        std::format("in converter {} missing bus or demand", uid());
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }

  bool add_to_lp(const SystemContext& sc, LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

private:
  OptTRealSched conversion_rate;

  STBIndexHolder conversion_rows;
  STBIndexHolder capacity_rows;

  ElementIndex<GeneratorLP> generator_index;
  ElementIndex<DemandLP> demand_index;
  ElementIndex<BatteryLP> battery_index;
};

}  // namespace gtopt
