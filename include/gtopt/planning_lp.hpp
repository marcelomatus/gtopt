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
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <gtopt/options_lp.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

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

using phase_systems_t = StrongIndexVector<PhaseIndex, SystemLP>;
using scene_phase_systems_t = StrongIndexVector<SceneIndex, phase_systems_t>;

class PlanningLP
{
public:
  PlanningLP(PlanningLP&&) noexcept = default;
  PlanningLP(const PlanningLP&) = default;
  PlanningLP& operator=(PlanningLP&&) noexcept = default;
  PlanningLP& operator=(const PlanningLP&) noexcept = default;
  ~PlanningLP() noexcept = default;

  /**
   * @brief Constructs a PlanningLP instance from planning data
   * @param planning The power system planning data
   * @param flat_opts Configuration options (default empty)
   */
  explicit PlanningLP(Planning planning, const FlatOptions& flat_opts = {});

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
   * @brief Gets the state variable map
   * @return Const reference to state variable map
   */
  [[nodiscard]] constexpr const state_variable_map_t& state_variables() const noexcept
  {
    return m_state_variable_map_;
  }

  /**
   * @brief Solves the linear programming problem
   * @param lp_opts Solver options (default empty)
   * @return Expected with solution status or error message
   */
  [[nodiscard]] std::expected<int, std::string> run_lp(
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

  // Add method with deducing this and perfect forwarding
  template<typename Self, typename StateVar>
  constexpr auto&& add_state_variable(this Self&& self, StateVar&& state_var)
  {
    const auto& key = state_var.key();
    auto&& map = std::forward<Self>(self).m_state_variable_map_;
    const auto [it, inserted] =
        map.try_emplace(key, std::forward<StateVar>(state_var));

    if (!inserted) {
      auto msg = fmt::format("duplicated variable {} in simulation map",
                             state_var.name());
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    return std::forward_like<Self>(it->second);
  }

  // Get method with deducing this for automatic const handling
  template<typename Key>
  [[nodiscard]] constexpr auto get_state_variable(Key&& key) const
      noexcept(noexcept(std::declval<state_variable_map_t>().find(key)))
  {
    using value_type = const StateVariable;

    using result_t = std::optional<std::reference_wrapper<value_type>>;

    auto&& map = m_state_variable_map_;
    const auto it = map.find(std::forward<Key>(key));

    return (it != map.end()) ? result_t {it->second} : std::nullopt;
  }

private:
  Planning m_planning_;
  OptionsLP m_options_;
  SimulationLP m_simulation_;

  scene_phase_systems_t m_systems_;

  state_variable_map_t m_state_variable_map_;
};

}  // namespace gtopt
