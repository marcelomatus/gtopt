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

#include <expected>

#include <boost/multi_array.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

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
class Simulation
{
public:
  /**
   * @brief Result type for simulation operations using std::expected for error
   * handling
   *
   * - Success: Returns 0 (int)
   * - Failure: Returns error message (std::string)
   */
  using result_type = std::expected<int, std::string>;

  /**
   * @brief Constructor that initializes the simulation with a system
   * @param system The power system to simulate
   *
   * Creates a SystemLP representation from the provided system and prepares
   * the simulation environment.
   */
  explicit Simulation(System system);

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
      System system,
      const std::optional<std::string>& lp_file,
      const std::optional<int>& use_lp_names,
      const std::optional<double>& matrix_eps,
      const std::optional<bool>& just_create);

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
  void create_lp();

private:
  /**
   * @brief Linear programming representation of the power system
   *
   * Holds all system components in their LP formulation and provides
   * methods for building the optimization model.
   */
  SystemLP system_lp;

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
  lp_matrix_t lp_matrix;
};

}  // namespace gtopt
