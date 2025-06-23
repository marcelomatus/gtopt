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
#include <gtopt/strong_index_vector.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

class PlanningLP;
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
  explicit SimulationLP(const Simulation& simulation, const OptionsLP& options);

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
  [[nodiscard]] constexpr const auto& scenes() const noexcept
  {
    return m_scene_array_;
  }

  /**
   * @brief Gets all scenario LP representations
   * @return Const reference to vector of ScenarioLP objects
   */
  [[nodiscard]] constexpr const auto& scenarios() const noexcept
  {
    return m_scenario_array_;
  }

  /**
   * @brief Gets all phase LP representations
   * @return Const reference to vector of PhaseLP objects
   */
  [[nodiscard]] constexpr const auto& phases() const noexcept
  {
    return m_phase_array_;
  }

  [[nodiscard]] constexpr const auto& blocks() const noexcept
  {
    return m_block_array_;
  }

  [[nodiscard]] constexpr const auto& stages() const noexcept
  {
    return m_stage_array_;
  }

  [[nodiscard]] constexpr auto previous_stage(const StageLP& stage)
  {
    if (stage.index() == StageIndex {0}) {
      throw std::out_of_range("No previous stage for the first stage");
    }
    return m_stage_array_[stage.index() - 1];
  }

  [[nodiscard]] constexpr auto prev_stage(const StageLP& stage) const noexcept
      -> std::pair<const StageLP*, const PhaseLP*>
  {
    if (stage.index() == StageIndex {0}) {
      if (const auto phase_index = stage.phase_index();
          phase_index == PhaseIndex {0})
      {
        return {nullptr, nullptr};
      }
      auto&& prev_phase = phases()[stage.phase_index() - 1];
      auto&& prev_stage = prev_phase.stages().back();
      return {&prev_stage, &prev_phase};
    }

    const auto prev_stage_index = StageIndex {stage.index() - 1};
    return {&m_stage_array_[prev_stage_index], nullptr};
  }

  // Get method with deducing this for automatic const handling
  using lp_key_t = StateVariable::LPKey;
  using state_variable_key_t = StateVariable::Key;
  using state_variable_map_t = flat_map<state_variable_key_t, StateVariable>;
  using global_variable_map_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, state_variable_map_t>>;

  // Add method with deducing this and perfect forwarding
  template<typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto add_state_variable(Key&& key, Index col)
      -> const StateVariable&
  {
    auto&& map =
        m_global_variable_map_[key.lp_key.scene_index][key.lp_key.phase_index];

    const auto [it, inserted] =
        map.try_emplace(std::forward<Key>(key), key.lp_key, col);

    if (!inserted) {
      auto msg = fmt::format("duplicated variable {}:{} in simulation map",
                             key.class_name,
                             key.col_name);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    return it->second;
  }

  [[nodiscard]]
  constexpr const auto& get_state_variables() noexcept
  {
    return m_global_variable_map_;
  }

  template<typename Self>
  [[nodiscard]]
  constexpr auto&& get_state_variables(this Self&& self,
                                       lp_key_t lp_key) noexcept
  {
    auto&& vec = std::forward<Self>(self).m_global_variable_map_;
    return vec[lp_key.scene_index][lp_key.phase_index];
  }

  /**
   * @brief Retrieves a state variable by its key
   * @tparam Self Type of the object (deduced using 'this' parameter)
   * @tparam Key Type of the key (default state_variable_key_t)
   * @param key The key to search for
   * @return Optional reference to the state variable if found (const or
   * non-const)
   */
  template<typename Self, typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto get_state_variable(this Self&& self, Key&& key) noexcept
  {
    using value_type =
        std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                           const StateVariable,
                           StateVariable>;
    using result_t = std::optional<std::reference_wrapper<value_type>>;

    auto&& map = std::forward<Self>(self).get_state_variables(key.lp_key);

    const auto it = map.find(std::forward<Key>(key));
    return (it != map.end()) ? result_t {it->second} : result_t {};
  }

private:
  std::reference_wrapper<const Simulation> m_simulation_;
  std::reference_wrapper<const OptionsLP> m_options_;
  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<SceneLP> m_scene_array_;

  global_variable_map_t m_global_variable_map_;
};

}  // namespace gtopt
