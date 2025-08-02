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

  /**
   * @brief Returns the underlying Line object with proper forwarding semantics
   * @tparam Self Deduced type of the calling object (const/non-const,
   * lvalue/rvalue)
   * @param self The calling object (implicit)
   * @return Reference to the Line object with same value category and
   * const-ness as self
   */
  [[nodiscard]] constexpr auto&& line(this auto&& self) noexcept
  {
    // Forward the object() call with same value category as self
    return std::forward_like<decltype(self)>(self.object());
  }

  [[nodiscard]] constexpr auto bus_a_sid() const noexcept -> BusLPSId
  {
    return BusLPSId {line().bus_a};
  }

  [[nodiscard]] constexpr auto bus_b_sid() const noexcept -> BusLPSId
  {
    return BusLPSId {line().bus_b};
  }

  [[nodiscard]] constexpr bool is_loop() const noexcept
  {
    return line().bus_a == line().bus_b;
  }

  explicit LineLP(Line pline, const InputContext& ic);

  bool add_to_lp(SystemContext& sc,
                 const ScenarioLP& scenario,
                 const StageLP& stage,
                 LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

private:
  OptTBRealSched tmax_ba;
  OptTBRealSched tmax_ab;
  OptTRealSched tcost;
  OptTRealSched lossfactor;
  OptTRealSched reactance;
  OptTRealSched voltage;

  STBIndexHolder<ColIndex> flowp_cols;
  STBIndexHolder<ColIndex> flown_cols;
  STBIndexHolder<RowIndex> capacityp_rows;
  STBIndexHolder<RowIndex> capacityn_rows;

  STBIndexHolder<RowIndex> theta_rows;
};

}  // namespace gtopt
