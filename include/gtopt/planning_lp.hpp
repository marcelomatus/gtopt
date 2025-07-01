/**
 * @file      planning.hpp
 * @brief     Linear programming planning for power system optimization
 * @author    marcelo
 * @date      Sun Apr  6 18:18:54 2025
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for creating, solving, and analyzing
 * linear programming models for power system planning.
 */

#pragma once

#include <expected>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <gtopt/error.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/**
 * @class PlanningLP
 * @brief Linear programming model for power system planning
 *
 * Encapsulates a linear programming formulation of a power system planning
 * problem, including system configurations, simulation scenarios, and solver
 * options.
 */

class PlanningLP
{
public:
  using phase_systems_t = StrongIndexVector<PhaseIndex, SystemLP>;
  using scene_phase_systems_t = StrongIndexVector<SceneIndex, phase_systems_t>;

private:
  constexpr static auto create_systems(System& system,
                                       SimulationLP& simulation,
                                       const OptionsLP& options,
                                       const FlatOptions& flat_opts)
      -> scene_phase_systems_t
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

public:
  /**
   * @brief Constructs a PlanningLP instance from planning data
   * @param planning The power system planning data
   * @param flat_opts Configuration options (default empty)
   */
  template<typename Planning>
    requires(std::is_same_v<std::remove_cvref_t<Planning>, gtopt::Planning>)
  explicit PlanningLP(Planning&& planning, const FlatOptions& flat_opts = {})
      : m_planning_(std::forward<Planning>(planning))
      , m_options_(m_planning_.options)
      , m_simulation_(m_planning_.simulation, m_options_)
      , m_systems_(create_systems(
            m_planning_.system, m_simulation_, m_options_, flat_opts))
  {
  }

  /**
   * @brief Gets the LP options configuration
   * @return Const reference to OptionsLP
   */
  [[nodiscard]] constexpr const OptionsLP& options() const noexcept
  {
    return m_options_;
  }

  /**
   * @brief Gets the underlying planning data
   * @return Const reference to Planning
   */
  [[nodiscard]] constexpr const Planning& planning() const noexcept
  {
    return m_planning_;
  }

  /**
   * @brief Gets the system LP representations
   * @return Const reference to vector of SystemLP
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& systems(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_systems_;
  }

  /**
   * @brief Gets the simulation scenarios
   * @return Const reference to vector of SimulationLP
   */
  /**
   * @brief Gets the simulation LP model
   * @return Const reference to SimulationLP
   */
  [[nodiscard]] constexpr const SimulationLP& simulation() const noexcept
  {
    return m_simulation_;
  }

  /**
   * @brief Solves the linear programming problem
   * @param lp_opts Solver options (default empty)
   * @return Expected with solution status or error message
   */
  [[nodiscard]] std::expected<int, Error> resolve(
      const SolverOptions& lp_opts = {});

  /**
   * @brief Writes the LP formulation to file
   * @param filename Output file path
   */
  void write_lp(const std::string& filename) const;

  /**
   * @brief Writes solution output (implementation-defined destination)
   */
  void write_out() const;

  template<typename Self>
  [[nodiscard]] constexpr auto&& system(this Self&& self,
                                        SceneIndex scene_index,
                                        PhaseIndex phase_index) noexcept
  {
    return std::forward<Self>(self).m_systems_[scene_index][phase_index];
  }

private:
  [[nodiscard]] std::expected<void, Error> resolve_scene_phases(
      SceneIndex scene_index,
      phase_systems_t& phase_systems,
      const SolverOptions& lp_opts);

  Planning m_planning_;
  OptionsLP m_options_;
  SimulationLP m_simulation_;

  scene_phase_systems_t m_systems_;
};

}  // namespace gtopt
