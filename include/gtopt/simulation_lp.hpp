/**
 * @file      simulation.hpp
 * @brief     Linear programming simulation for power system optimization
 * @date      Sun Apr  6 18:18:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for creating, solving, and analyzing
 * linear programming models for power system optimization.
 */
#pragma once

#include <expected>

#include <boost/multi_array.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system.hpp>

namespace gtopt
{
class SystemLP;
/**
 * @class Simulation
 * @brief Coordinates the creation, solution, and analysis of power system
 * optimization models
 *
 * The Simulation class is responsible for:
 * - Converting a power system model to a linear programming formulation
 * - Solving the linear program using a solver interface
 * - Processing and organizing optimization results
 * - Handling different time periods and scenarios in the optimization
 *
 * It serves as the main entry point for running power system optimization
 * studies.
 */
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
  constexpr std::vector<ScenarioLP> create_scenario_array();

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
  /**
   * @brief Result type for simulation operations using std::expected for error
   * handling
   *
   * - Success: Returns 0 (int)
   * - Failure: Returns error message (std::string)
   */
  using result_type = std::expected<int, std::string>;

  /** @brief Move constructor */
  SimulationLP(SimulationLP&&) noexcept = default;

  /** @brief Copy constructor */
  SimulationLP(const SimulationLP&) = default;

  /** @brief Default constructor deleted - must initialize with a Simulation */
  SimulationLP() = delete;

  /** @brief Move assignment operator */
  SimulationLP& operator=(SimulationLP&&) noexcept = default;

  /** @brief Copy assignment operator */
  SimulationLP& operator=(const SimulationLP&) noexcept = default;

  /** @brief Destructor */
  ~SimulationLP() = default;

  /**
   * @brief Constructor that initializes the SimulationLP with a Simulation
   * @param simulation
   *
   * Creates a SimulationLP representation from the provided simulation and
   * prepares the simulation environment.
   */
  explicit SimulationLP(Simulation simulation, const OptionsLP& options);

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

#if NONE
  /**
   * @brief Runs linear programming optimization on a power system
   * @param system The power system to optimize
   * @param lp_file Optional file path to write the LP model (for debugging)
   * @param use_lp_names Optional flag for naming LP variables:
   *        - 0: No names
   *        - 1: Include names in LP model
   *        - 2: Include names and name maps
   * @param matrix_eps Optional epsilon value for matrix filtering (coefficients
   * smaller than this are treated as zero)
   * @param just_create Optional flag to only create the model without solving
   * (for debugging)
   * @return Success (0) or error message
   *
   * This static method provides a simplified interface to create, solve, and
   * analyze a power system optimization model in a single call. It handles the
   * complete workflow from system definition to optimization results.
   */
  [[nodiscard]] static result_type run_lp(
      const System& system,
      const std::optional<std::string>& lp_file,
      const std::optional<int>& use_lp_names,
      const std::optional<double>& matrix_eps,
      const std::optional<bool>& just_create);

#endif
  /**
   * @brief Creates the linear programming matrix for all system components
   * @throws std::runtime_error if system_lp is not properly initialized
   *
   * This method builds the complete linear programming problem by:
   * 1. Creating LP variables and constraints for each system component
   * 2. Building the constraint matrix for all scenarios and stages
   * 3. Preparing the objective function and bounds
   *
   * The result is stored in the lp_matrix member variable, organized by
   * scenario and stage.
   */
  void create_linear_problems(System system);

private:
  Simulation m_simulation_;
  std::reference_wrapper<const OptionsLP> m_options_;
  std::vector<SystemLP> m_system_lps_;

  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<SceneLP> m_scene_array_;

  /**
   * @brief Linear programming representation of the power system
   *
   * Holds all system components in their LP formulation and provides
   * methods for building the optimization model.
   */

  // using system_lp_matrix_t = boost::multi_array<SystemLP, 2>;
  // system_lp_matrix_t system_lp_matrix;

  // SystemLP system_lp;

  /**
   * @brief Two-dimensional array of LinearProblem objects
   *
   * This matrix contains separate LinearProblem instances for each
   * scenario and stage combination, allowing for:
   * - Parallel model building
   * - Separate solution of subproblems
   * - Organized handling of results by scenario/stage
   *
   * The first dimension corresponds to scenarios, and the second to stages.
   */
  using lp_matrix_t = boost::multi_array<LinearProblem, 2>;
};

}  // namespace gtopt
