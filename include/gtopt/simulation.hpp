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

class Simulation
{
public:
  using result_type = std::expected<int, std::string>;

  /**
   * @brief Constructor that initializes the simulation with a system
   * @param system The power system to simulate
   */
  explicit Simulation(System system);

  /**
   * @brief Runs linear programming optimization on a power system
   * @param system The power system to optimize
   * @param lp_file Optional file path to write the LP model
   * @param use_lp_names Optional flag for naming LP variables (0=none, 1=names,
   * 2=name maps)
   * @param matrix_eps Optional epsilon value for matrix filtering
   * @param just_create Optional flag to only create the model without solving
   * @return Success (0) or error message
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
   */
  void create_lp();

private:
  SystemLP system_lp;

  using lp_matrix_t = boost::multi_array<LinearProblem, 2>;
  lp_matrix_t lp_matrix;
};

}  // namespace gtopt
