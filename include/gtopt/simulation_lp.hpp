/**
 * @file      simulation.hpp
 * @brief     Linear programming simulation for power system planning
 * @date      Sun Apr  6 18:18:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for creating, solving, and analyzing
 * linear programming models for power system planning.
 */
#pragma once

#include <expected>
#include <functional>

#include <gtopt/block_lp.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage_lp.hpp>

namespace gtopt
{

class SimulationLP
{
  /**
   * @brief Creates LP representation of blocks from system blocks
   * @return Vector of BlockLP objects
   */
  constexpr std::vector<BlockLP> create_block_array();

  /**
   * @brief Creates LP representation of stages from system stages
   * @return Vector of StageLP objects
   */
  constexpr std::vector<StageLP> create_stage_array();

  /**
   * @brief Creates LP representation of scenarios from system scenarios
   * @return Vector of ScenarioLP objects
   */
  constexpr std::vector<ScenarioLP> create_scenario_array(const Scene& scene);

  /**
   * @brief Creates LP representation of phases from system phases
   * @return Vector of PhaseLP objects
   */
  constexpr std::vector<PhaseLP> create_phase_array();

  /**
   * @brief Creates LP representation of scenes from system scenes
   * @return Vector of SceneLP objects
   */
  constexpr std::vector<SceneLP> create_scene_array();

  /**
   * @brief Validates components for consistency
   * @throws std::runtime_error if validation fails
   */
  constexpr void validate_components();

public:
  /** @brief Move constructor */
  SimulationLP(SimulationLP&&) noexcept = default;

  /** @brief Copy constructor */
  SimulationLP(const SimulationLP&) = default;

  /** @brief Default constructor deleted - must initialize with a Simulation */
  SimulationLP() = delete;

  /** @brief Move assignment operator */
  constexpr SimulationLP& operator=(SimulationLP&&) noexcept = default;

  /** @brief Copy assignment operator */
  constexpr SimulationLP& operator=(const SimulationLP&) noexcept = default;

  /** @brief Destructor */
  ~SimulationLP() = default;

  /**
   * @brief Constructor that initializes the SimulationLP with a Simulation
   * @param simulation
   *
   * Creates a SimulationLP representation from the provided simulation and
   * prepares the simulation environment.
   */
  explicit SimulationLP(const Simulation& simulation,
                        const OptionsLP& options,
                        const Scene& scene = {});

  template<typename Self>
  [[nodiscard]] constexpr auto&& simulation(this Self& self)
  {
    return std::forward<Self>(self).m_simulation_.get();
  }

  /**
   * @brief Gets system options LP representation
   * @return Reference to the options object
   */
  [[nodiscard]] constexpr const auto& options() const
  {
    return m_options_.get();
  }

  /**
   * @brief Gets all scene LP objects
   * @return Reference to the vector of SceneLP objects
   */
  [[nodiscard]] constexpr const auto& scenes() const { return m_scene_array_; }

  /**
   * @brief Gets all scenario LP objects
   * @return Reference to the vector of ScenarioLP objects
   */
  [[nodiscard]] constexpr const auto& scenarios() const
  {
    return m_scenario_array_;
  }

  /**
   * @brief Gets all phase LP objects
   * @return Reference to the vector of PhaseLP objects
   */
  [[nodiscard]] constexpr const auto& phases() const { return m_phase_array_; }

  /**
   * @brief Gets all stage LP objects
   * @return Reference to the vector of StageLP objects
   */
  [[nodiscard]] constexpr const auto& stages() const { return m_stage_array_; }

  /**
   * @brief Gets all block LP objects
   * @return Reference to the vector of BlockLP objects
   */
  [[nodiscard]] constexpr const auto& blocks() const { return m_block_array_; }

private:
  std::reference_wrapper<const Simulation> m_simulation_;
  std::reference_wrapper<const OptionsLP> m_options_;

  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<SceneLP> m_scene_array_;
};

}  // namespace gtopt
