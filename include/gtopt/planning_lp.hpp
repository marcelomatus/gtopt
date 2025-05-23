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
#include <vector>

#include <gtopt/options_lp.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_options.hpp>
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
  [[nodiscard]] constexpr const auto& systems() const noexcept
  {
    return m_systems_;
  }

  /**
   * @brief Gets the simulation scenarios
   * @return Const reference to vector of SimulationLP
   */
  [[nodiscard]] constexpr const auto& simulations() const noexcept
  {
    return m_simulations_;
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

private:
  Planning m_planning_;
  OptionsLP m_options_;
  StrongIndexVector<PhaseIndex, SimulationLP> m_simulations_;
  StrongIndexVector<PhaseIndex, SystemLP> m_systems_;
};

}  // namespace gtopt
