/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

namespace gtopt
{
namespace
{

[[nodiscard]] PlanningLP::scene_phase_systems_t create_systems(
    System& system,
    SimulationLP& simulation,
    const OptionsLP& options,
    const FlatOptions& flat_opts)
{
  system.setup_reference_bus(options);

  auto&& scenes = simulation.scenes();

  PlanningLP::scene_phase_systems_t all_systems;
  all_systems.reserve(scenes.size());

  for (auto&& scene : scenes) {
    auto&& phases = simulation.phases();
    PlanningLP::phase_systems_t phase_systems;
    phase_systems.reserve(phases.size());
    for (auto&& phase : phases) {
      phase_systems.emplace_back(system, simulation, phase, scene, flat_opts);
    }
    all_systems.push_back(std::move(phase_systems));
  }

  return all_systems;
}

}  // namespace

PlanningLP::PlanningLP(Planning planning, const FlatOptions& flat_opts)
    : m_planning_(std::move(planning))
    , m_options_(m_planning_.options)
    , m_simulation_(m_planning_.simulation, m_options_)
    , m_systems_(create_systems(
          m_planning_.system, m_simulation_, m_options_, flat_opts))
{
}

void PlanningLP::write_lp(const std::string& filename) const
{
  for (auto&& phase_systems : m_systems_) {
    for (auto&& system : phase_systems) {
      system.write_lp(filename);
    }
  }
}

void PlanningLP::write_out() const
{
  for (auto&& phase_systems : m_systems_) {
    for (auto&& system : phase_systems) {
      system.write_out();
    }
  }
}

auto PlanningLP::resolve(const SolverOptions& lp_opts)
    -> std::expected<int, std::string>
{
  try {
    bool status = true;
    for (auto&& [scene_index, phase_systems] : enumerate<SceneIndex>(systems()))
    {
      for (auto&& [phase_index, system_sp] :
           enumerate<PhaseIndex>(phase_systems))
      {
        if (auto result = system_sp.resolve(lp_opts); !result) {
          status = false;
          break;
        }
        auto&& state_vars = simulation().get_state_variables({
            .scene_index = scene_index,
            .phase_index = phase_index,
        });

        auto&& lp_interface = system_sp.linear_interface();
        const auto& solution_vector = lp_interface.get_col_sol();

        for (auto&& [var_name, state_var] : state_vars) {
          const double solution_value = solution_vector[state_var.col()];

          for (auto&& dependent_var : state_var.dependent_variables()) {
            auto& dependent_lp =
                system(dependent_var.lp_key).linear_interface();
            dependent_lp.set_col(dependent_var.col, solution_value);
          }
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

    return {status};  // Success
  } catch (const std::exception& e) {
    // Handle unexpected errors gracefully
    return std::unexpected(std::string("Unexpected error: ") + e.what());
  }
}

}  // namespace gtopt
