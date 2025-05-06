/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <filesystem>

#include <gtopt/planning_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

#include "gtopt/simulation.hpp"
#include "gtopt/solver_options.hpp"

namespace gtopt
{

constexpr std::vector<SimulationLP> PlanningLP::create_simulations() const
{
  const auto& simulation = m_planning_.simulation;
  const auto& scenes = simulation.scene_array;
  const auto n_size = scenes.size();

  std::vector<SimulationLP> simulations;
  simulations.reserve(n_size);

  for (auto&& scene : scenes) {
    simulations.emplace_back(simulation, options(), scene);
  }

  return simulations;
}

constexpr std::vector<SystemLP> PlanningLP::create_systems(
    const FlatOptions& flat_opts)
{
  auto& system = m_planning_.system;
  const auto& simulations = m_simulations_;
  const auto n_size = simulations.size();

  std::vector<SystemLP> systems;
  systems.reserve(n_size);

  system.setup_reference_bus(options());

  for (auto&& simulation_lp : simulations) {
    systems.emplace_back(system, simulation_lp, flat_opts);
  }

  return systems;
}

PlanningLP::PlanningLP(Planning planning, const FlatOptions& flat_opts)
    : m_planning_(std::move(planning))
    , m_options_(m_planning_.options)
    , m_simulations_(create_simulations())
    , m_systems_(create_systems(flat_opts))
{
}

void PlanningLP::write_lp(const std::string& filename) const
{
  for (auto&& system : m_systems_) {
    system.write_lp(filename);
  }
}

void PlanningLP::write_out() const
{
  for (auto&& system_lp : m_systems_) {
    system_lp.write_out();
  }
}

auto PlanningLP::run_lp(const SolverOptions& lp_opts)
    -> std::expected<int, std::string>
{
  try {
    // Solve the planning problem
    {
      bool status = true;
      for (auto&& system_lp : m_systems_) {
        status = system_lp.resolve(lp_opts);
        if (!status) {
          break;
        }
      }

      // Handle infeasible or unbounded problems
      if (!status) {
        // On error, write the problematic model to a file for debugging
        try {
          write_lp("error");
        } catch (const std::exception& e) {
          SPDLOG_WARN(
              std::format("Failed to write error LP file: {}", e.what()));
        }

        // Return detailed error message
        constexpr auto unexpected =
            std::unexpected("Problem is not feasible, check the error.lp file");
        SPDLOG_INFO(unexpected.error());
        return unexpected;
      }
    }

    return {0};  // Success
  } catch (const std::exception& e) {
    // Handle unexpected errors gracefully
    return std::unexpected(std::string("Unexpected error: ") + e.what());
  }
}

}  // namespace gtopt
