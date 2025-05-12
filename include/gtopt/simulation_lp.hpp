/**
 * @file      simulation.hpp
 * @brief     Linear programming simulation for power system planning
 * @author    marcelo
 * @copyright BSD-3-Clause
 * @version   1.0.0
 * @date      Sun Apr  6 18:18:54 2025
 *
 * Provides functionality for creating, solving, and analyzing linear
 * programming models for power system planning with strong exception safety
 * guarantees.
 */

#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <gtopt/block_lp.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/state_variable.hpp>

namespace gtopt
{

/**
 * @class SimulationLP
 * @brief Linear programming representation of a power system simulation
 *
 * Encapsulates the LP transformation of a power system simulation model,
 * providing access to all components in their LP form. Maintains references
 * to the original simulation and options objects.
 */
class SimulationLP
{
public:
  SimulationLP(SimulationLP&&) noexcept = default;
  SimulationLP(const SimulationLP&) = default;
  SimulationLP() = delete;
  SimulationLP& operator=(SimulationLP&&) noexcept = default;
  SimulationLP& operator=(const SimulationLP&) noexcept = default;
  ~SimulationLP() noexcept = default;

  /**
   * @brief Constructs a SimulationLP from a Simulation
   * @param simulation Reference to the base simulation model
   * @param options Reference to LP solver options
   * @param scene Optional scene for scenario creation (default empty)
   * @throws std::runtime_error If component validation fails
   * @throws std::bad_alloc If memory allocation fails
   */
  explicit SimulationLP(const Simulation& psimulation,
                       const OptionsLP& poptions,
                       const Scene& pscene = {}) noexcept(false);

  // Accessors
  /**
   * @brief Gets the underlying simulation model
   * @return Reference to the simulation object
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& simulation(this Self& self) noexcept
  {
    return std::forward<Self>(self).m_simulation_.get();
  }

  /**
   * @brief Gets the LP solver options
   * @return Const reference to the options object
   */
  [[nodiscard]] constexpr const OptionsLP& options() const noexcept
  {
    return m_options_.get();
  }

  /**
   * @brief Gets all scene LP representations
   * @return Const reference to vector of SceneLP objects
   */
  [[nodiscard]] constexpr const std::vector<SceneLP>& scenes() const noexcept
  {
    return m_scene_array_;
  }

  /**
   * @brief Gets all scenario LP representations
   * @return Const reference to vector of ScenarioLP objects
   */
  [[nodiscard]] constexpr const std::vector<ScenarioLP>& scenarios()
      const noexcept
  {
    return m_scenario_array_;
  }

  /**
   * @brief Gets all phase LP representations
   * @return Const reference to vector of PhaseLP objects
   */
  [[nodiscard]] constexpr const std::vector<PhaseLP>& phases() const noexcept
  {
    return m_phase_array_;
  }

  /**
   * @brief Gets all stage LP representations
   * @return Const reference to vector of StageLP objects
   */
  [[nodiscard]] constexpr const std::vector<StageLP>& stages() const noexcept
  {
    return m_stage_array_;
  }

  /**
   * @brief Gets all block LP representations
   * @return Const reference to vector of BlockLP objects
   */
  [[nodiscard]] constexpr const std::vector<BlockLP>& blocks() const noexcept
  {
    return m_block_array_;
  }

  // Add method with deducing this and perfect forwarding
  template<typename Self, typename StateVar>
  constexpr auto&& add_state_variable(this Self&& self,
                                      StateVar&& state_var) noexcept
  {
    const auto& key = state_var.key();
    auto&& map = std::forward<Self>(self).m_state_variable_map_;
    const auto [it, inserted] =
        map.try_emplace(key, std::forward<StateVar>(state_var));

    if (!inserted) {
      it->second = std::forward<StateVar>(state_var);
    }

    return std::forward_like<Self>(it->second);
  }

  // Get method with deducing this for automatic const handling
  template<typename Self, typename Key>
  constexpr auto get_state_variable(this Self&& self, Key&& key) 
    noexcept(noexcept(std::declval<state_variable_map_t>().find(key)))
  {
    using value_type =
        std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                           const StateVariable,
                           StateVariable>;

    using result_t = std::optional<std::reference_wrapper<value_type>>;

    auto&& map = std::forward<Self>(self).m_state_variable_map_;
    const auto it = map.find(std::forward<Key>(key));

    return (it != map.end()) ? result_t {it->second} : std::nullopt;
  }

private:
  /**
   * @brief Validates all components for consistency
   * @throws std::runtime_error If any validation check fails
   */
  void validate_components();

  // Data members
  std::reference_wrapper<const Simulation> m_simulation_;
  std::reference_wrapper<const OptionsLP> m_options_;

  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<SceneLP> m_scene_array_;

  state_variable_map_t m_state_variable_map_;
};

}  // namespace gtopt
