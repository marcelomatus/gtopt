/**
 * @file      simulation.cpp
 * @brief     Implementation of linear programming simulation
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements power system optimization through linear programming
 * techniques, supporting multi-phase, multi-scene scenarios. It handles the
 * full optimization workflow from model creation through solution and result
 * extraction.
 *
 * @details The simulation framework supports complex modeling structures with:
 * - Multiple phases (time horizon segments)
 * - Multiple scenes (system states or operating conditions)
 * - Multiple scenarios (stochastic representations)
 * - Multiple stages (time steps within phases)
 */

#include <expected>
#include <filesystem>
#include <string>

#include <boost/multi_array/base.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

#include "gtopt/scenario.hpp"
#include "gtopt/stage.hpp"

namespace gtopt
{
/**
 * @brief Constructs a simulation object with the given system
 * @param system Power system model to be simulated
 *
 * Initializes the simulation with the provided system model, which contains
 * all components (buses, generators, lines, etc.) and their attributes.
 */
Simulation::Simulation(System system)
    : system_lp(std::move(system))
{
}

/**
 * @brief Runs a linear programming optimization for the system
 * @param system Power system model to optimize
 * @param lp_file Optional path to write the LP formulation to a file
 * @param use_lp_names Optional level of naming detail in LP file (0=none,
 * 1=names, 2=name maps)
 * @param matrix_eps Optional numerical tolerance for matrix coefficients
 * @param just_create Optional flag to only create the model without solving
 * @return Success value (0) or error description
 *
 * @details This function performs the complete optimization workflow:
 * 1. Creates the linear programming formulation from the system model
 * 2. Flattens the representation for the solver interface
 * 3. Optionally writes the LP formulation to a file
 * 4. Solves the optimization problem
 * 5. Writes results back to the system context
 *
 * If any errors occur during the process, the function returns an error
 * description via std::unexpected.
 *
 * Performance is tracked via stopwatch measurements at key points.
 */
auto Simulation::run_lp(System system,
                        const std::optional<std::string>& lp_file,
                        const std::optional<int>& use_lp_names,
                        const std::optional<double>& matrix_eps,
                        const std::optional<bool>& just_create) -> result_type
{
  spdlog::stopwatch sw;  // Timer for performance tracking

  try {
    // Initialize system LP representation
    SystemLP system_lp {std::move(system)};
    SPDLOG_INFO(fmt::format("system_lp creation {}", sw));

    LinearInterface lp_interface;
    {
      // Create the linear problem with appropriate size reservation
      constexpr size_t reserve_size =
          1'024;  // Pre-allocate memory for efficiency
      LinearProblem linear_problem(system_lp.name(), reserve_size);

      // Add system constraints to linear problem
      system_lp.add_to_lp(linear_problem);
      SPDLOG_INFO(fmt::format("lp creation {}", sw));

      // Configure options for flattening with parameter validation
      const auto eps = matrix_eps.value_or(0);  // Default to exact matching
      const auto lp_names = use_lp_names.value_or(0);  // Default to no names

      // Configure flattening options based on input parameters
      FlatOptions flat_opts;
      flat_opts.eps = eps;  // Coefficient epsilon tolerance
      flat_opts.col_with_names = lp_names > 0;  // Include variable names?
      flat_opts.row_with_names = lp_names > 0;  // Include constraint names?
      flat_opts.col_with_name_map =
          lp_names > 1;  // Include variable name mapping?
      flat_opts.row_with_name_map =
          lp_names > 1;  // Include constraint name mapping?
      flat_opts.reserve_matrix =
          false;  // Don't pre-reserve matrix (already done)
      flat_opts.reserve_factor = 2;  // Memory reservation factor

      // Flatten the problem and load it into the solver interface
      auto flat_lp = linear_problem.to_flat(flat_opts);
      SPDLOG_INFO(fmt::format("lp flattening {}", sw));

      lp_interface.load_flat(flat_lp);
      SPDLOG_INFO(fmt::format("lp loading {}", sw));
    }

    // Write LP file if requested (for debugging or external solving)
    if (lp_file) {
      try {
        const std::filesystem::path lpath {lp_file.value()};
        lp_interface.write_lp(lpath.stem());
        SPDLOG_INFO(fmt::format("lp writing {}", sw));
      } catch (const std::exception& e) {
        // Log warning but continue with optimization
        const auto msg =
            std::format("Exception when writing LP file: {}", e.what());
        SPDLOG_WARN(msg);
      }
    }

    // Early exit if only creating the model without solving
    if (just_create.value_or(false)) {
      SPDLOG_INFO("just creating the problem, exiting now");
      return {0};
    }

    // Solve the optimization problem
    {
      const LPOptions lp_opts {};  // Default solver options
      const auto status = lp_interface.resolve(lp_opts);

      // Handle infeasible or unbounded problems
      if (!status) {
        // On error, write the problematic model to a file for debugging
        try {
          lp_interface.write_lp("error");
        } catch (const std::exception& e) {
          const auto msg =
              std::format("Failed to write error LP file: {}", e.what());
          SPDLOG_WARN(msg);
        }

        // Return detailed error message
        constexpr auto e =
            std::unexpected("Problem is not feasible, check the error.lp file");
        SPDLOG_CRITICAL(e.error());
        return e;
      }

      SPDLOG_INFO(fmt::format("lp solving {}", sw));
    }

    // Write the optimization results back to the system context
    {
      system_lp.write_out(lp_interface);
      SPDLOG_INFO(fmt::format("write output {}", sw));
    }

    return {0};  // Success
  } catch (const std::exception& e) {
    // Handle unexpected errors gracefully
    return std::unexpected(std::string("Unexpected error: ") + e.what());
  }
}

/**
 * @brief Creates the multi-dimensional LP problem structure
 *
 * @details This method initializes the multi-dimensional matrix of LP problems
 * based on the system's phase and scene structure. It organizes optimization
 * problems in a hierarchical structure:
 * - Phases (top level time horizons)
 * - Scenes (system configurations)
 * - Scenarios (stochastic variants)
 * - Stages (time steps)
 *
 * For each valid combination, it adds the corresponding system components
 * to the appropriate LP subproblem.
 */
void Simulation::create_lp()
{
  // Use type aliases for better readability
  using index_t = boost::multi_array_types::index;

  // Get dimensions once for efficiency
  const auto n_phase = static_cast<index_t>(system_lp.phases().size());
  const auto n_scene = static_cast<index_t>(system_lp.scenes().size());

  // Validate dimensions
  if (n_phase == 0 || n_scene == 0) {
    const auto msg = std::format("Empty phases or scenes in create_lp");
    SPDLOG_WARN(msg);
    return;
  }

  // Pre-allocate the multi-dimensional matrix of LP problems
  lp_matrix.resize(boost::extents[n_phase][n_scene]);

  // Process all active scenes (independent system states)
  for (const auto& [scene_index, scene] :
       enumerate_active<SceneIndex>(system_lp.scenes()))
  {
    // Process all active phases for each scenario (time horizons)
    for (const auto& [phase_index, phase] :
         enumerate_active<PhaseIndex>(system_lp.phases()))
    {
      // Get reference to the specific LP subproblem for this phase-scene
      // combination
      auto& lp = lp_matrix[phase_index][scene_index];

      // Process all active scenarios in current scene (stochastic variants)
      for (const auto& [scenario_index, scenario] :
           enumerate_active<ScenarioIndex>(scene.scenarios()))
      {
        // Process all active stages in current phase (time steps)
        for (const auto& [stage_index, stage] :
             enumerate_active<StageIndex>(phase.stages()))
        {
          // Add system components to the linear program for this specific
          // combination of phase, scene, scenario, and stage
          system_lp.add_to_lp(lp, stage_index, stage, scenario_index, scenario);
        }
      }
    }
  }
}

}  // namespace gtopt
