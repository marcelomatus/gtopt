/**
 * @file      simulation.hpp
 * @brief     Linear programming simulation for power system planning
 * @author    marcelo
 * @copyright BSD-3-Clause
 * @version   1.0.0
 * @date      Sun Apr  6 18:18:54 2025
 *
 * Provides functionality for creating, solving, and analyzing linear
 * programming models for power system planning with strong exception safety
 * guarantees.
 */

#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <gtopt/ampl_variable.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

class PlanningLP;
/**
 * @class SimulationLP
 * @brief Linear programming representation of a power system simulation
 *
 * Encapsulates the LP transformation of a power system simulation model,
 * providing access to all components in their LP form. Maintains references
 * to the original simulation and options objects.
 */
class SimulationLP
{
public:
  SimulationLP(SimulationLP&&) noexcept = default;
  SimulationLP(const SimulationLP&) = default;

  SimulationLP& operator=(SimulationLP&&) noexcept = default;
  SimulationLP& operator=(const SimulationLP&) noexcept = default;
  ~SimulationLP() noexcept = default;

  /**
   * @brief Constructs a SimulationLP from a Simulation
   * @param simulation Reference to the base simulation model
   * @param options Reference to LP solver options
   * @throws std::runtime_error If component validation fails
   * @throws std::bad_alloc If memory allocation fails
   */
  explicit SimulationLP(const Simulation& simulation,
                        const PlanningOptionsLP& options);

  // Accessors
  /**
   * @brief Gets the underlying simulation model
   * @return Reference to the simulation object
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& simulation(this Self& self) noexcept
  {
    return std::forward<Self>(self).m_simulation_.get();
  }

  /**
   * @brief Gets the LP solver options
   * @return Const reference to the options object
   */
  [[nodiscard]] constexpr const PlanningOptionsLP& options() const noexcept
  {
    return m_options_.get();
  }

  /**
   * @brief Gets all scene LP representations
   * @return Const reference to vector of SceneLP objects
   */
  [[nodiscard]] constexpr const auto& scenes() const noexcept
  {
    return m_scene_array_;
  }

  /**
   * @brief Gets all scenario LP representations
   * @return Const reference to vector of ScenarioLP objects
   */
  [[nodiscard]] constexpr const auto& scenarios() const noexcept
  {
    return m_scenario_array_;
  }

  /**
   * @brief Gets all phase LP representations
   * @return Const reference to vector of PhaseLP objects
   */
  [[nodiscard]] constexpr const auto& phases() const noexcept
  {
    return m_phase_array_;
  }

  [[nodiscard]] constexpr const auto& blocks() const noexcept
  {
    return m_block_array_;
  }

  [[nodiscard]] constexpr const auto& stages() const noexcept
  {
    return m_stage_array_;
  }

  /**
   * @brief Gets the aperture definitions from the underlying Simulation
   * @return Const reference to the aperture_array
   */
  [[nodiscard]] constexpr const auto& apertures() const noexcept
  {
    return m_simulation_.get().aperture_array;
  }

  [[nodiscard]] constexpr auto previous_stage(const StageLP& stage)
  {
    if (stage.index() == StageIndex {0}) {
      throw std::out_of_range("No previous stage for the first stage");
    }
    return m_stage_array_[stage.index() - 1];
  }

  [[nodiscard]] constexpr auto prev_stage(const StageLP& stage) const noexcept
      -> std::pair<const StageLP*, const PhaseLP*>
  {
    if (stage.index() == StageIndex {0}) {
      if (const auto phase_index = stage.phase_index();
          phase_index == PhaseIndex {0})
      {
        return {nullptr, nullptr};
      }
      auto&& prev_phase = phases()[stage.phase_index() - 1];
      auto&& prev_stage = prev_phase.stages().back();
      return {&prev_stage, &prev_phase};
    }

    const auto prev_stage_index = stage.index() - StageIndex {1};
    return {&m_stage_array_[prev_stage_index], nullptr};
  }

  // Get method with deducing this for automatic const handling
  using lp_key_t = StateVariable::LPKey;
  using state_variable_key_t = StateVariable::Key;
  using state_variable_map_t = flat_map<state_variable_key_t, StateVariable>;
  using global_variable_map_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, state_variable_map_t>>;

  // Add method with deducing this and perfect forwarding
  template<typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto add_state_variable(Key&& key,
                                    ColIndex col,
                                    double scost,
                                    double var_scale,
                                    LpContext context) -> const StateVariable&
  {
    auto&& map =
        m_global_variable_map_[key.lp_key.scene_index][key.lp_key.phase_index];

    const auto [it, inserted] = map.try_emplace(std::forward<Key>(key),
                                                key.lp_key,
                                                col,
                                                scost,
                                                var_scale,
                                                std::move(context));

    if (!inserted) {
      const auto msg =
          std::format("duplicated variable {}:{} in simulation map",
                      key.class_name,
                      key.col_name);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }

    return it->second;
  }

  [[nodiscard]]
  constexpr const auto& state_variables() noexcept
  {
    return m_global_variable_map_;
  }

  template<typename Self>
  [[nodiscard]]
  constexpr auto&& state_variables(this Self&& self,
                                   SceneIndex scene_index,
                                   PhaseIndex phase_index) noexcept
  {
    auto&& vec = std::forward<Self>(self).m_global_variable_map_;
    return vec[scene_index][phase_index];
  }

  /**
   * @brief Retrieves a state variable by its key
   * @param self  The object instance (deduced via explicit object parameter)
   * @param key   The key to search for
   * @return Optional reference to the state variable if found (const or
   * non-const)
   */
  template<typename Self, typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto state_variable(this Self&& self, Key&& key) noexcept
  {
    using value_type =
        std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                           const StateVariable,
                           StateVariable>;
    using result_t = std::optional<std::reference_wrapper<value_type>>;

    auto&& map = std::forward<Self>(self).state_variables(
        key.lp_key.scene_index, key.lp_key.phase_index);

    const auto it = map.find(std::forward<Key>(key));
    return (it != map.end()) ? result_t {it->second} : result_t {};
  }

  // ── PAMPL / user-constraint variable registry ─────────────────────────────
  //
  // Each LP element calls `add_ampl_variable` from its `add_to_lp` once per
  // (scenario, stage) after populating its per-block column map, and once
  // per stage-level scalar column (capainst, eini, efin, ...).  The resolver
  // later looks them up via `find_ampl_col` without any per-element-type
  // dispatch.
  //
  // Names (for `generator("G1")` style references) are registered once per
  // element via `register_ampl_element`.

  /// Register a per-block variable map (e.g., generator.generation).
  /// @param class_name  canonical lowercase class name ("generator", "line")
  /// @param element_uid element's Uid
  /// @param attribute   PAMPL attribute name ("generation", "flowp", ...)
  /// @param scenario_uid / stage_uid the (scenario, stage) this map is for
  /// @param block_cols  reference to the element's BIndexHolder for this
  ///                    (scenario, stage).  Stored by pointer — lifetime
  ///                    must extend through the end of LP solving.
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         ScenarioUid scenario_uid,
                         StageUid stage_uid,
                         const BIndexHolder<ColIndex>& block_cols)
  {
    m_ampl_variables_.insert_or_assign(
        AmplVariableKey {
            .class_name = std::string {class_name},
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .block_cols = &block_cols,
            .stage_col = ColIndex {unknown_index},
        });
  }

  /// Register a stage-level scalar column (e.g., eini, efin, capainst).
  /// Returns the same value for every block in the stage.
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         ScenarioUid scenario_uid,
                         StageUid stage_uid,
                         ColIndex stage_col)
  {
    m_ampl_variables_.insert_or_assign(
        AmplVariableKey {
            .class_name = std::string {class_name},
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .block_cols = nullptr,
            .stage_col = stage_col,
        });
  }

  /// Look up a registered variable.  Returns nullopt if the
  /// (class, uid, attribute, scenario, stage, block) combination was not
  /// registered — e.g., when the column is conditional (soft_emin, drain)
  /// and was never created.
  [[nodiscard]] std::optional<ColIndex> find_ampl_col(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      BlockUid block_uid) const
  {
    const auto it = m_ampl_variables_.find(AmplVariableKey {
        .class_name = std::string {class_name},
        .element_uid = element_uid,
        .attribute = attribute,
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
    });
    if (it == m_ampl_variables_.end()) {
      return std::nullopt;
    }
    return it->second.col_at(block_uid);
  }

  /// Register an element's name so that user-expressions like
  /// `generator("G1")` resolve "G1" to its Uid.
  void register_ampl_element(std::string_view class_name,
                             std::string_view element_name,
                             Uid element_uid)
  {
    m_ampl_element_names_.insert_or_assign(
        AmplElementNameKey {std::string {class_name},
                            std::string {element_name}},
        element_uid);
  }

  /// Look up an element Uid by (class_name, name).
  [[nodiscard]] std::optional<Uid> lookup_ampl_element_uid(
      std::string_view class_name, std::string_view element_name) const
  {
    const auto it = m_ampl_element_names_.find(AmplElementNameKey {
        std::string {class_name}, std::string {element_name}});
    return (it != m_ampl_element_names_.end()) ? std::optional<Uid> {it->second}
                                               : std::nullopt;
  }

private:
  std::reference_wrapper<const Simulation> m_simulation_;
  std::reference_wrapper<const PlanningOptionsLP> m_options_;
  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<SceneLP> m_scene_array_;

  global_variable_map_t m_global_variable_map_;

  // PAMPL variable registry — populated by each LP element's add_to_lp
  // and queried by element_column_resolver.cpp.
  AmplVariableMap m_ampl_variables_;
  AmplElementNameMap m_ampl_element_names_;
};

}  // namespace gtopt
