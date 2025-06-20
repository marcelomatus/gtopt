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
#include <gtopt/linear_interface.hpp>
#include <gtopt/phase_lp.hpp>

namespace gtopt
{

/**
 * @class StateVariable
 * @brief Represents a decision variable in an optimization problem
 *
 * Tracks a variable's association with a phase and stage in the power system
 * model, along with its position in the optimization matrix. Each variable has:
 * - A unique name
 * - Associated stage and phase indices
 * - Column range in the LP formulation
 */
class StateVariable
{
public:
  using key_t = std::tuple<NameView, PhaseIndex>;

  constexpr StateVariable() = default;

  /**
   * @brief Constructs a valid state variable
   * @param name Variable name
   * @param phase_index Associated phase index
   * @param phase_index Associated phase index
   * @param first_col First column in optimization matrix
   * @param last_col Last column in optimization matrix
   *
   * @note The class supports both move and copy operations. Move operations
   * leave the source object in a valid but unspecified state (name empty,
   * indices -1).
   */
  constexpr explicit StateVariable(Name name,
                                   const PhaseLP& phase,
                                   Index col) noexcept
      : m_name_(std::move(name))
      , m_phase_index_(phase.index())
      , m_col_(col)
  {
  }

  /// @return Variable name view
  [[nodiscard]] constexpr NameView name() const noexcept { return m_name_; }

  /// @return Associated phase index
  [[nodiscard]] constexpr PhaseIndex phase_index() const noexcept
  {
    return m_phase_index_;
  }

  /// @return First column index in optimization matrix
  [[nodiscard]] constexpr Index col() const noexcept { return m_col_; }

  /// @return Unique key tuple for this variable (name, stage, phase)
  [[nodiscard]] constexpr auto key() const noexcept
  {
    return key_t {name(), phase_index()};
  }

  [[nodiscard]] constexpr bool reg_client(Name name,
                                       std::shared_ptr<LinearInterface> lp,
                                       Index col) noexcept
  {
    auto res = m_clients_.emplace(std::move(name),
                                 std::pair{std::move(lp), col});
    return res.second;
  }

private:
  Name m_name_;  ///< Variable name
  PhaseIndex m_phase_index_ {unknown_index};  ///< Associated phase index
  Index m_col_ {unknown_index};  ///< Column index

  flat_map<Name, std::pair<std::shared_ptr<LinearInterface>, Index>> m_clients_;
};

// Type aliases for cleaner usage
using state_variable_key_t = StateVariable::key_t;
using state_variable_map_t = flat_map<state_variable_key_t, StateVariable>;

}  // namespace gtopt
