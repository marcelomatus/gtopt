/**
 * @file      planning.hpp
 * @brief     Linear programming planning for power system planning
 * @date      Sun Apr  6 18:18:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functionality for creating, solving, and analyzing
 * linear programming models for power system planning.
 */
#pragma once

#include <expected>

#include <gtopt/options_lp.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

#include "gtopt/solver_options.hpp"

namespace gtopt
{

class PlanningLP
{
  [[nodiscard]] constexpr std::vector<SimulationLP> create_simulations() const;
  [[nodiscard]] constexpr std::vector<SystemLP> create_systems(
      const FlatOptions& flat_opts);

public:
  /** @brief Move constructor */
  PlanningLP(PlanningLP&&) noexcept = default;

  /** @brief Copy constructor */
  PlanningLP(const PlanningLP&) = default;

  /** @brief Default constructor deleted - must initialize with a Planning */
  PlanningLP() = delete;

  /** @brief Move assignment operator */
  PlanningLP& operator=(PlanningLP&&) noexcept = default;

  /** @brief Copy assignment operator */
  PlanningLP& operator=(const PlanningLP&) noexcept = default;

  /** @brief Destructor */
  ~PlanningLP() = default;

  /**
   * @brief Constructor that initializes the PlanningLP with a Planning
   * @param planning
   *
   * Creates a PlanningLP representation from the provided planning and
   * prepares the planning environment.
   */
  explicit PlanningLP(Planning planning, const FlatOptions& flat_opts = {});

  /**
   * @brief Gets system options LP representation
   * @return Reference to the options object
   */
  [[nodiscard]] constexpr const auto& options() const { return m_options_; }
  [[nodiscard]] constexpr const auto& planning() const { return m_planning_; }
  [[nodiscard]] constexpr const auto& systems() const { return m_systems_; }
  [[nodiscard]] constexpr const auto& simulations() const
  {
    return m_simulations_;
  }

  std::expected<int, std::string> run_lp(const SolverOptions& lp_opts = {});
  void write_lp(const std::string& filename) const;
  void write_out() const;

private:
  Planning m_planning_;
  OptionsLP m_options_;
  std::vector<SimulationLP> m_simulations_;
  std::vector<SystemLP> m_systems_;
};

}  // namespace gtopt
