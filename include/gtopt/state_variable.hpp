/**
 * @file      state_variable.hpp
 * @brief     State variable representation for optimization problems
 * @author    marcelo
 * @date      Fri May  9 18:31:14 2025
 * @copyright BSD-3-Clause
 *
 * Defines the StateVariable class representing decision variables in
 * optimization models. Each variable is associated with specific phases and
 * stages in the power system model and tracks its position in the optimization
 * matrix.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage.hpp>

namespace gtopt
{

class StateVariable
{
public:
  struct Key
  {
    ScenarioUid scenario_uid;
    StageUid stage_uid;
    std::string_view class_name;
    Uid uid;
    std::string_view col_name;

    auto operator<=>(const Key&) const = default;
  };

  [[nodiscard]]
  static auto key(std::string_view class_name,
                  Uid uid,
                  std::string_view col_name,
                  StageUid stage_uid = StageUid {unknown_uid},
                  ScenarioUid scenario_uid = ScenarioUid {unknown_uid}) -> Key
  {
    return {.scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
            .class_name = class_name,
            .uid = uid,
            .col_name = col_name};
  }

  template<typename Element>
  [[nodiscard]]
  static auto key(const Element& element,
                  std::string_view col_name,
                  StageUid stage_uid = StageUid {unknown_uid},
                  ScenarioUid scenario_uid = ScenarioUid {unknown_uid}) -> Key
  {
    return key(
        element.class_name(), element.uid(), col_name, stage_uid, scenario_uid);
  }

  constexpr explicit StateVariable(LinearProblem& lp, Index col) noexcept
      : m_lp_(lp)
      , m_col_(col)
  {
  }

  [[nodiscard]] constexpr Index col() const noexcept { return m_col_; }

  using state_client_t =
      std::tuple<std::reference_wrapper<LinearProblem>, Index>;

  constexpr auto&& add_client(LinearProblem& lp, Index col) noexcept
  {
    return m_clients_.emplace_back(lp, col);
  }

private:
  std::reference_wrapper<LinearProblem> m_lp_;
  Index m_col_ {unknown_index};

  std::vector<state_client_t> m_clients_;
};

}  // namespace gtopt
