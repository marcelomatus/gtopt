/**
 * @file      system_lp.hpp
 * @brief     Header for SystemLP class that handles linear programming
 * formulation
 * @date      Sat Mar 29 19:16:40 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the SystemLP class, which coordinates the formulation of
 * the entire power system optimization model as a linear programming problem.
 * It manages collections of all system components and their LP representations,
 * and provides methods to build the complete LP model from these components.
 */

#pragma once

#include <tuple>
#include <vector>

#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile_lp.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_context.hpp>

#include "gtopt/linear_interface.hpp"

namespace gtopt
{

class LinearInterface;

template<typename T,
         typename SC = SystemContext,
         typename OC = OutputContext,
         typename LP = LinearProblem>
concept AddToLP = requires(T obj, const SC& sc, OC& oc, LP& lp) {
  { obj.add_to_lp(sc, lp) } -> std::same_as<bool>;
  { obj.add_to_output(oc) } -> std::same_as<bool>;
};

static_assert(AddToLP<BusLP>);

/**
 * @class SystemLP
 * @brief Manages the linear programming formulation of a complete power system
 * model
 *
 * The SystemLP class is responsible for:
 * - Converting System components to their LP representations
 * - Managing collections of all LP components
 * - Coordinating the creation of variables and constraints across all
 * components
 * - Providing a unified interface to add all system constraints to a
 * LinearProblem
 * - Managing output of optimization results
 *
 * It serves as the central coordinator for the optimization model formulation,
 * handling time structure (blocks, stages, scenarios), grid components, and
 * their interactions in the LP formulation.
 */
class SystemLP
{
  /**
   * @brief Creates LP representation of blocks from system blocks
   * @return Vector of BlockLP objects
   */
  std::vector<BlockLP> create_block_array();

  /**
   * @brief Creates LP representation of stages from system stages
   * @return Vector of StageLP objects
   */
  std::vector<StageLP> create_stage_array();

  /**
   * @brief Creates LP representation of scenarios from system scenarios
   * @return Vector of ScenarioLP objects
   */
  std::vector<ScenarioLP> create_scenario_array();

  /**
   * @brief Creates LP representation of phases from system phases
   * @return Vector of PhaseLP objects
   */
  std::vector<PhaseLP> create_phase_array();

  /**
   * @brief Creates LP representation of scenes from system scenes
   * @return Vector of SceneLP objects
   */
  std::vector<SceneLP> create_scene_array();

  /**
   * @brief Validates system components for consistency
   * @throws std::runtime_error if validation fails
   */
  void validate_system_components();

  /**
   * @brief Sets up a reference bus for voltage angle if needed
   */
  void setup_reference_bus();

  /**
   * @brief Initializes collections of LP components from system components
   */
  void initialize_collections();

public:
  /** @brief Move constructor */
  SystemLP(SystemLP&&) noexcept = default;

  /** @brief Copy constructor */
  SystemLP(const SystemLP&) = default;

  /** @brief Default constructor deleted - must initialize with a System */
  SystemLP() = delete;

  /** @brief Move assignment operator */
  SystemLP& operator=(SystemLP&&) noexcept = default;

  /** @brief Copy assignment operator */
  SystemLP& operator=(const SystemLP&) noexcept = default;

  /** @brief Destructor */
  ~SystemLP() = default;

  /**
   * @brief Constructs a SystemLP from a System
   * @param psystem The system to convert to LP representation
   */
  explicit SystemLP(System psystem = {});

  /**
   * @brief Gets all scene LP objects
   * @return Reference to the vector of SceneLP objects
   */
  constexpr const auto& scenes() const { return m_scene_array_; }

  /**
   * @brief Gets all scenario LP objects
   * @return Reference to the vector of ScenarioLP objects
   */
  constexpr const auto& scenarios() const { return m_scenario_array_; }

  /**
   * @brief Gets all phase LP objects
   * @return Reference to the vector of PhaseLP objects
   */
  constexpr const auto& phases() const { return m_phase_array_; }

  /**
   * @brief Gets all stage LP objects
   * @return Reference to the vector of StageLP objects
   */
  constexpr const auto& stages() const { return m_stage_array_; }

  /**
   * @brief Gets all block LP objects
   * @return Reference to the vector of BlockLP objects
   */
  constexpr const auto& blocks() const { return m_block_array_; }

  /**
   * @brief Gets system options LP representation
   * @return Reference to the SystemOptionsLP object
   */
  constexpr const auto& options() const { return m_options_; }

  /**
   * @brief Adds an element to its corresponding collection
   * @tparam Element Type of element to add
   * @param e Element to add
   * @return Result of the push_back operation on the collection
   */
  template<typename Element>
  constexpr auto push_back(Element&& e)
  {
    return std::get<Collection<Element>>(m_collections_)
        .push_back(std::forward<Element>(e));
  }

  /**
   * @brief Gets all elements of a specific type
   * @tparam Element Type of elements to retrieve
   * @return Reference to the elements container
   */
  template<typename Element>
  constexpr auto&& elements()
  {
    return std::get<Collection<Element>>(m_collections_).elements();
  }

  /**
   * @brief Gets all elements of a specific type (const version)
   * @tparam Element Type of elements to retrieve
   * @return Const reference to the elements container
   */
  template<typename Element>
  constexpr auto&& elements() const
  {
    return std::get<Collection<Element>>(m_collections_).elements();
  }

  /**
   * @brief Gets the index of an element by its ID
   * @tparam Element Type of element
   * @tparam Id ID type template
   * @param id Element ID
   * @return Index of the element
   */
  template<typename Element, template<typename> class Id>
  constexpr auto element_index(const Id<Element>& id) const
  {
    return std::get<Collection<Element>>(m_collections_).element_index(id);
  }

  /**
   * @brief Gets an element by its ID
   * @tparam Element Type of element
   * @tparam Id ID type template
   * @param id Element ID
   * @return Reference to the element
   */
  template<typename Element, template<typename> class Id>
  constexpr auto&& element(const Id<Element>& id)
  {
    return std::get<Collection<Element>>(m_collections_).element(id);
  }

  /**
   * @brief Gets an element by its ID (const version)
   * @tparam Element Type of element
   * @tparam Id ID type template
   * @param id Element ID
   * @return Const reference to the element
   */
  template<typename Element, template<typename> class Id>
  constexpr auto&& element(const Id<Element>& id) const
  {
    return std::get<Collection<Element>>(m_collections_).element(id);
  }

  /**
   * @brief Gets the system name
   * @return System name
   */
  const auto& name() const { return m_system_.name; }

  /**
   * @brief Adds LP formulation for a specific stage and scenario
   * @param lp Linear problem to add constraints to
   * @param stage_index Index of the stage
   * @param stage Stage LP representation
   * @param scenario_index Index of the scenario
   * @param scenario Scenario LP representation
   */
  void add_to_lp(LinearProblem& lp,
                 const StageIndex& stage_index,
                 const StageLP& stage,
                 const ScenarioIndex& scenario_index,
                 const ScenarioLP& scenario);

  /**
   * @brief Adds the complete LP formulation for all active stages and scenarios
   * @param lp Linear problem to add constraints to
   */
  void add_to_lp(LinearProblem& lp);

  /**
   * @brief Writes optimization results to output context
   * @param li Linear interface containing the solved problem
   */
  void write_out(const LinearInterface& li) const;

private:
  System m_system_;
  SystemOptionsLP m_options_;

  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<SceneLP> m_scene_array_;

  SystemContext system_context;
  InputContext input_context;

  std::tuple<Collection<BusLP>,
             Collection<DemandLP>,
             Collection<GeneratorLP>,
             Collection<LineLP>,
             Collection<GeneratorProfileLP>,
             Collection<DemandProfileLP>,
             Collection<BatteryLP>,
             Collection<ConverterLP>,
             Collection<ReserveZoneLP>,
             Collection<ReserveProvisionLP>>
      m_collections_;
};

}  // namespace gtopt
