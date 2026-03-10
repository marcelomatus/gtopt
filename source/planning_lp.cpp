/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <filesystem>
#include <ranges>

#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_solver.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

void PlanningLP::write_lp(const std::string& filename) const
{
  try {
    for (auto&& phase_systems : m_systems_) {
      for (auto&& system : phase_systems) {
        try {
          system.write_lp(filename);
        } catch (const std::exception& e) {
          SPDLOG_ERROR(
              std::format("Failed to write LP for system: {}", e.what()));
          throw;
        }
      }
    }
  } catch (const std::exception& e) {
    SPDLOG_ERROR(
        "Failed to write LP file {}: {}", filename, std::string(e.what()));
    throw;
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

std::expected<void, Error> PlanningLP::resolve_scene_phases(
    SceneIndex scene_index,
    phase_systems_t& phase_systems,
    const SolverOptions& lp_opts)
{
  for (auto&& [phase_index, system_sp] : enumerate<PhaseIndex>(phase_systems)) {
    if (auto result = system_sp.resolve(lp_opts); !result) {
      // On error, write the problematic model to the log directory for
      // debugging
      const auto log_dir = m_options_.log_directory();
      std::filesystem::create_directories(log_dir);
      const auto filename =
          (std::filesystem::path(log_dir)
           / std::format("error_{}_{}", scene_index, phase_index))
              .string();
      system_sp.write_lp(filename);

      auto error = std::move(result.error());
      error.message += std::format(
          ", failed at scene {} phase {}", scene_index, phase_index);

      return std::unexpected(std::move(error));
    }

    // update state variable dependents with the last solution
    const auto& solution_vector = system_sp.linear_interface().get_col_sol();

    for (auto&& state_var :
         simulation().state_variables(scene_index, phase_index)
             | std::views::values)
    {
      const double solution_value = solution_vector[state_var.col()];

      for (auto&& dep_var : state_var.dependent_variables()) {
        auto& target_system =
            system(dep_var.scene_index(), dep_var.phase_index());
        target_system.linear_interface().set_col(dep_var.col(), solution_value);
      }
    }
  }

  return {};
}

auto PlanningLP::resolve(const SolverOptions& lp_opts)
    -> std::expected<int, Error>
{
  auto solver = make_planning_solver(m_options_.solver_type(),
                                     m_options_.cut_sharing_mode(),
                                     m_options_.cut_directory(),
                                     m_options_.log_directory(),
                                     m_options_.sddp_api_enabled(),
                                     m_options_.output_directory(),
                                     m_options_.sddp_max_iterations(),
                                     m_options_.sddp_convergence_tol(),
                                     m_options_.sddp_elastic_penalty(),
                                     m_options_.sddp_elastic_mode());
  return solver->solve(*this, lp_opts);
}

}  // namespace gtopt
