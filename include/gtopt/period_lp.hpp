/**
 * @file      period_lp.hpp
 * @brief     Header of
 * @date      Wed Mar 26 12:10:25 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <functional>
#include <numeric>
#include <span>

#include <gtopt/basic_types.hpp>
#include <gtopt/period.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class PeriodLP
{
public:
  using StageSpan = std::span<const StageLP>;
  using StageIndexes = std::vector<StageIndex>;
  using StageIndexSpan = std::span<const StageIndex>;

  PeriodLP() = default;

  template<class Stages>
  explicit PeriodLP(Period pperiod, const Stages& pstages)
      : period(std::move(pperiod))
      , stage_span(
            std::span(pstages).subspan(period.first_stage, period.count_stage))
      , stage_indexes(period.count_stage)
      , span_duration(std::transform_reduce(stage_span.begin(),
                                            stage_span.end(),
                                            0,
                                            std::plus(),
                                            [](auto&& b)
                                            { return b.duration(); }))
  {
    std::iota(  // NOLINT
        stage_indexes.begin(),
        stage_indexes.end(),
        StageIndex {});
  }

  [[nodiscard]] constexpr auto duration() const { return span_duration; }
  [[nodiscard]] constexpr auto is_active() const
  {
    return period.active.value_or(true);
  }
  [[nodiscard]] constexpr auto uid() const { return PeriodUid {period.uid}; }
  [[nodiscard]] constexpr auto&& stages() const { return stage_span; }
  [[nodiscard]] constexpr auto indexes() const
  {
    return StageIndexSpan {stage_indexes};
  }

private:
  Period period;
  StageSpan stage_span;
  StageIndexes stage_indexes;

  double span_duration {0};
};

}  // namespace gtopt
