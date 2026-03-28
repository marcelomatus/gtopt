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

#include <string>
#include <utility>

#include <gtopt/error.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/strong_index_vector.hpp>
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
  using phase_systems_t = StrongIndexVector<PhaseIndex, SystemLP>;
  using scene_phase_systems_t = StrongIndexVector<SceneIndex, phase_systems_t>;

private:
  static auto create_systems(System& system,
                             SimulationLP& simulation,
                             const PlanningOptionsLP& options,
                             const LpBuildOptions& flat_opts)
      -> scene_phase_systems_t;

public:
  /**
   * @brief Constructs a PlanningLP instance from planning data
   * @param planning The power system planning data
   * @param flat_opts Configuration options (default empty)
   */
  template<typename PlanningT>
    requires(std::is_same_v<std::remove_cvref_t<PlanningT>, gtopt::Planning>)
  explicit PlanningLP(PlanningT&& planning,
                      const LpBuildOptions& flat_opts = {})
      : m_planning_(std::forward<PlanningT>(planning))  // NOLINT
      , m_options_(m_planning_.options)
      , m_simulation_(m_planning_.simulation, m_options_)
      , m_systems_(create_systems(
            m_planning_.system, m_simulation_, m_options_, flat_opts))
  {
  }

  /**
   * @brief Gets the LP options configuration
   * @return Const reference to PlanningOptionsLP
   */
  [[nodiscard]] constexpr const PlanningOptionsLP& options() const noexcept
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

  // ── SDDP solve summary ──────────────────────────────────────────────────

  /**
   * @brief Summary of the last SDDP solve (populated by the SDDP solver).
   *
   * Carries the final-iteration gap metrics so that `write_out()` can emit
   * them as extra columns in `solution.csv`.  For monolithic solves this
   * struct is left at its default (all-zero / false) values.
   */
  struct SddpSummary
  {
    double gap {0.0};  ///< Final primary gap (UB−LB)/max(1,|UB|)
    double gap_change {1.0};  ///< Final stationary gap-change metric
    double lower_bound {0.0};  ///< Final lower bound
    double upper_bound {0.0};  ///< Final upper bound
    int iterations {0};  ///< Number of training iterations completed
    bool converged {false};  ///< True if any convergence criterion was met
    bool stationary_converged {
        false,
    };  ///< True if stationary criterion triggered convergence
    bool statistical_converged {
        false,
    };  ///< True if CI-based statistical criterion triggered convergence
  };

  /** @brief Populate the SDDP summary (called by the SDDP solver). */
  void set_sddp_summary(SddpSummary summary) noexcept
  {
    m_sddp_summary_ = summary;
  }

  /** @brief Read the SDDP summary (populated after a successful SDDP solve). */
  [[nodiscard]] const SddpSummary& sddp_summary() const noexcept
  {
    return m_sddp_summary_;
  }

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

  friend class MonolithicMethod;

  Planning m_planning_;
  PlanningOptionsLP m_options_;
  SimulationLP m_simulation_;

  scene_phase_systems_t m_systems_;
  SddpSummary m_sddp_summary_ {};
};

}  // namespace gtopt
