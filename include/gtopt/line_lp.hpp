/**
 * @file      line_lp.hpp
 * @brief     Header of
 * @date      Sat Mar 29 19:02:33 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_object_lp.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/line.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

class LineLP : public CapacityObjectLP<Line>
{
public:
  constexpr static std::string_view ClassName = "Line";

  using CapacityBase = CapacityObjectLP<Line>;

  [[nodiscard]] constexpr auto&& line() { return object(); }
  [[nodiscard]] constexpr auto&& line() const { return object(); }

  [[nodiscard]] auto bus_a() const { return BusLPSId {line().bus_a}; }
  [[nodiscard]] auto bus_b() const { return BusLPSId {line().bus_b}; }

  explicit LineLP(const InputContext& ic, Line pline);

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

private:
  OptTBRealSched tmin;
  OptTBRealSched tmax;
  OptTRealSched tcost;
  OptTRealSched lossfactor;
  OptTRealSched reactance;
  OptTRealSched voltage;

  STBIndexHolder flowp_cols;
  STBIndexHolder flown_cols;
  STBIndexHolder capacityp_rows;
  STBIndexHolder capacityn_rows;

  STBIndexHolder theta_rows;
};

}  // namespace gtopt
