/**
 * @file      planning.cpp
 * @brief     Implementation of linear programming planning
 * @date      Sun Apr  6 22:05:37 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 */

#include <ranges>

#include <gtopt/planning_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

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
              fmt::format("Failed to write LP for system: {}", e.what()));
          throw;
        }
      }
    }
  } catch (const std::exception& e) {
    SPDLOG_ERROR(fmt::format(
        "Failed to write LP file {}: {}", filename, std::string(e.what())));
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
      // On error, write the problematic model to a file for debugging
      auto filename =
          fmt::format("error_scene_{}_phase_{}.lp", scene_index, phase_index);
      system_sp.write_lp(filename);

      auto&& error = result.error();
      result.error().message += fmt::format(
          ", failed at scene {} phase {}", scene_index, phase_index);

      return std::unexpected(error);
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
  WorkPoolConfig pool_config {};

  const double cpu_factor = 1.25;
  pool_config.max_threads = static_cast<int>(
      std::lround((cpu_factor * std::thread::hardware_concurrency())));
  pool_config.max_cpu_threshold = static_cast<int>(
      100.0 - (50.0 / static_cast<double>(pool_config.max_threads)));

  AdaptiveWorkPool pool(pool_config);
  pool.start();

  try {
    using future_t = std::future<std::expected<void, Error>>;

    std::vector<future_t> futures;
    futures.reserve(systems().size());

    for (auto&& [scene_index, phase_systems] : enumerate<SceneIndex>(systems()))
    {
      auto result = pool.submit(
          [&]
          {
            return resolve_scene_phases(scene_index, phase_systems, lp_opts);
          });
      futures.push_back(std::move(result.value()));
    }

    // Check all futures for errors
    for (auto& future : futures) {
      if (auto result = future.get(); !result) {
        return std::unexpected(std::move(result.error()));
      }
    }

    return futures.size();  // Return number of successfully processed scenes

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::InternalError,
        .message = fmt::format("Unexpected error in resolve: {}", e.what())});
  }
}

}  // namespace gtopt
