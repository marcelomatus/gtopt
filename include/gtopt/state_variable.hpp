/**
 * @file      state_variable.hpp
 * @brief     State variable representation for optimization problems
 * @author    marcelo
 * @date      Fri May  9 18:31:14 2025
 * @copyright BSD-3-Clause
 *
 * Defines the StateVariable class representing decision variables in
 * optimization models. Each variable is associated with specific phases and
 * scenes in the power system model and tracks its position in the optimization
 * matrix.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>

namespace gtopt
{

/**
 * @class StateVariable
 * @brief Represents a decision variable in an optimization problem
 *
 * Tracks a variable's association with a phase and scene in the power system
 * model, along with its position in the optimization matrix. Each variable has:
 * - A unique name
 * - Associated scene and phase indices
 * - Column range in the LP formulation
 */
class StateVariable
{
public:
  using key_t = std::tuple<NameView, SceneIndex, PhaseIndex>;

  constexpr StateVariable() = default;

  /**
   * @brief Constructs a valid state variable
   * @param name Variable name
   * @param scene_index Associated scene index
   * @param phase_index Associated phase index
   * @param first_col First column in optimization matrix
   * @param last_col Last column in optimization matrix
   * @throws std::invalid_argument For invalid column negative indices
   */
  constexpr StateVariable(Name name,
                          SceneIndex scene_index,
                          PhaseIndex phase_index,
                          Index first_col,
                          Index last_col) noexcept(false)
      : m_name_(std::move(name))
      , m_scene_index_(scene_index)
      , m_phase_index_(phase_index)
      , m_first_col_(first_col)
      , m_last_col_(last_col)
  {
  }

  /// @return Variable name view
  [[nodiscard]] constexpr NameView name() const noexcept { return m_name_; }

  /// @return Associated phase index
  [[nodiscard]] constexpr PhaseIndex phase_index() const noexcept
  {
    return m_phase_index_;
  }

  /// @return Associated scene index
  [[nodiscard]] constexpr SceneIndex scene_index() const noexcept
  {
    return m_scene_index_;
  }

  /// @return First column index in optimization matrix
  [[nodiscard]] constexpr Index first_col() const noexcept
  {
    return m_first_col_;
  }

  /// @return Last column index in optimization matrix
  [[nodiscard]] constexpr Index last_col() const noexcept
  {
    return m_last_col_;
  }

  /// @return Unique key tuple for this variable (name, scene, phase)
  [[nodiscard]] constexpr auto key() const noexcept
  {
    return key_t {name(), scene_index(), phase_index()};
  }

private:
  Name m_name_;  ///< Variable name
  SceneIndex m_scene_index_;  ///< Associated scene index
  PhaseIndex m_phase_index_;  ///< Associated phase index
  Index m_first_col_ {-1};  ///< First column index (invalid when negative)
  Index m_last_col_ {-1};  ///< Last column index (invalid when negative)
};

// Type aliases for cleaner usage
using state_variable_key_t = StateVariable::key_t;
using state_variable_map_t = flat_map<state_variable_key_t, StateVariable>;

}  // namespace gtopt
