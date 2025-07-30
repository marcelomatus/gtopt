/**
 * @file      junction_lp.hpp
 * @brief     Header of
 * @date      Tue Jul 29 23:08:29 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <gtopt/junction.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class JunctionLP : public ObjectLP<Junction>
{
public:
  constexpr static std::string_view ClassName = "Junction";

  explicit JunctionLP([[maybe_unused]] const InputContext& ic,
                      Junction pjunction)
      : ObjectLP<Junction>(std::move(pjunction))
  {
  }

  [[nodiscard]]
  constexpr auto&& junction() const noexcept
  {
    return ObjectLP<Junction>::object();
  }

  [[nodiscard]] constexpr auto drain() const noexcept
  {
    return junction().drain.value_or(false);
  }

  [[nodiscard]] constexpr auto is_drain() const noexcept
  {
    return junction().drain.value_or(false);
  }

  bool add_to_lp(const SystemContext& sc, LinearProblem& lp);

  bool add_to_output(OutputContext& out) const;

  [[nodiscard]] auto&& balance_rows_at(const ScenarioUid scenario,
                                       const StageUid stage) const
  {
    return balance_rows.at({scenario, stage});
  }

private:
  STBIndexHolder<RowIndex> balance_rows;
  STBIndexHolder<ColIndex> drain_cols;
};

}  // namespace gtopt
