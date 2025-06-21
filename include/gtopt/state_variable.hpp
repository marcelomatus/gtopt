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

#include <utility>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase_lp.hpp>

namespace gtopt
{

using state_variable_key_t = std::tuple<std::string, StageUid>;

class StateVariable
{
public:
  template<typename... Var>
  static constexpr auto key(const StageLP& stage, Var... var)
      -> state_variable_key_t
  {
    return {fmt::format("{}", fmt::join(std::forward_as_tuple(var...), "_")),
            stage.uid()};
  }

  constexpr explicit StateVariable(Name name,
                                   LinearInterface& lp,
                                   Index col,
                                   const StageLP& stage) noexcept
      : m_name_(std::move(name))
      , m_lp_(lp)
      , m_col_(col)
      , m_stage_uid_(stage.uid())
  {
  }

  /// @return Variable name view
  [[nodiscard]] constexpr NameView name() const noexcept { return m_name_; }

  /// @return First column index in optimization matrix
  [[nodiscard]] constexpr Index col() const noexcept { return m_col_; }

  using state_client_t =
      std::pair<std::reference_wrapper<LinearInterface>, Index>;

  [[nodiscard]] constexpr bool register_client(Name name,
                                               LinearInterface& lp,
                                               Index col) noexcept
  {
    return m_clients_.emplace(std::move(name), state_client_t {lp, col}).second;
  }

  template<typename... Var>
  static constexpr auto key(const StageLP& stage, Var... var)
      -> state_variable_key_t
  {
    return {fmt::format("{}", fmt::join(std::forward_as_tuple(var...), "_")),
            stage.uid()};
  }

private:
  Name m_name_;
  std::reference_wrapper<LinearInterface> m_lp_;
  Index m_col_ {unknown_index};
  StageUid m_stage_uid_ {unknown_uid};

  flat_map<Name, state_client_t> m_clients_;
};

}  // namespace gtopt
