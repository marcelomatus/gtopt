/**
 * @file      system_lp.hpp
 * @brief     Header for SystemLP class that handles linear programming
 * formulation
 * @date      Sat Mar 29 19:16:40 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the SystemLP class, which coordinates the formulation of
 * the entire power system planning model as a linear programming problem.
 * It manages collections of all system components and their LP representations,
 * and provides methods to build the complete LP model from these components.
 */

#pragma once

#include <functional>

#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_context.hpp>

#include "gtopt/solver_options.hpp"

namespace gtopt
{

class LinearInterface;

template<typename T>
concept AddToLP = requires(T obj,
                           const SystemContext& system_context,
                           const ScenarioIndex& scenario_index,
                           const StageIndex& stage_index,
                           LinearProblem& lp,
                           OutputContext& output_context) {
  {
    obj.add_to_lp(system_context, scenario_index, stage_index, lp)
  } -> std::same_as<bool>;
  { obj.add_to_output(output_context) } -> std::same_as<bool>;
};

static_assert(AddToLP<BusLP>);
static_assert(AddToLP<DemandLP>);
static_assert(AddToLP<GeneratorLP>);
static_assert(AddToLP<LineLP>);
static_assert(AddToLP<GeneratorProfileLP>);
static_assert(AddToLP<DemandProfileLP>);
static_assert(AddToLP<BatteryLP>);
static_assert(AddToLP<ConverterLP>);
static_assert(AddToLP<ReserveZoneLP>);
static_assert(AddToLP<ReserveProvisionLP>);

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
 * - Managing output of planning results
 *
 * It serves as the central coordinator for the planning model formulation,
 * handling time structure (blocks, stages, scenarios), grid components, and
 * their interactions in the LP formulation.
 */
class SystemLP
{
  using collections_t = std::tuple<Collection<BusLP>,
                                   Collection<DemandLP>,
                                   Collection<GeneratorLP>,
                                   Collection<LineLP>,
                                   Collection<GeneratorProfileLP>,
                                   Collection<DemandProfileLP>,
                                   Collection<BatteryLP>,
                                   Collection<ConverterLP>,
                                   Collection<ReserveZoneLP>,
                                   Collection<ReserveProvisionLP>>;

  /**
   * @brief Initializes collections of LP components from system components
   */
  constexpr collections_t create_collections();
  constexpr std::vector<LinearInterface> create_linear_interfaces(
      const SimulationLP& simulation, const FlatOptions& flat_opts);

public:
  /** @brief Move constructor */
  SystemLP(SystemLP&&) noexcept = default;

  /** @brief Copy constructor */
  SystemLP(const SystemLP&) = default;

  /** @brief Default constructor deleted - must initialize with a System */
  SystemLP() = delete;

  /** @brief Move assignment operator */
  constexpr SystemLP& operator=(SystemLP&&) noexcept = default;

  /** @brief Copy assignment operator */
  constexpr SystemLP& operator=(const SystemLP&) noexcept = default;

  /** @brief Destructor */
  ~SystemLP() = default;

  /**
   * @brief Constructs a SystemLP from a System
   * @param psystem The system to convert to LP representation
   */
  explicit SystemLP(const System& system,
                    const SimulationLP& simulation,
                    const FlatOptions& flat_opts = {});

  /**
   * @brief Gets system options LP representation
   * @return Reference to the options object
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& system(this Self& self)
  {
    return std::forward<Self>(self).m_system_.get();
  }

  template<typename Self>
  [[nodiscard]] constexpr auto&& linear_interfaces(this Self& self)
  {
    return std::forward<Self>(self).m_linear_interfaces_;
  }

  [[nodiscard]] constexpr const auto& system_context() const
  {
    return m_system_context_;
  }

  [[nodiscard]] constexpr const auto& options() const
  {
    return system_context().options();
  }

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
  [[nodiscard]] const auto& name() const { return system().name; }

  void write_lp(const std::string& filename) const;

  bool resolve(const SolverOptions& lp_opts);

  void write_out() const;

  /**
   * @brief Adds LP formulation for a specific stage and scenario
   * @param lp Linear problem to add constraints to
   * @param system_context
   */
  void add_to_lp(const SystemContext& system_context,
                 const ScenarioIndex& scenario_index,
                 const StageIndex& stage_index,
                 LinearProblem& lp);

  /**
   * @brief Writes planning results to output context
   * @param li Linear interface containing the solved problem
   */
  constexpr void write_out(const SystemContext& system_context,
                           const LinearInterface& li) const;

private:
  //
  // Data members
  //

  std::reference_wrapper<const System> m_system_;
  SystemContext m_system_context_;
  collections_t m_collections_;
  std::vector<LinearInterface> m_linear_interfaces_;
};

}  // namespace gtopt
