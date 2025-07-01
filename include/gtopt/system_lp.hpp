/**
 * @file      system_lp.hpp
 * @brief     SystemLP class for power system linear programming formulation
 * @author    marcelo
 * @copyright BSD-3-Clause
 * @date      Sat Mar 29 19:16:40 2025
 *
 * Defines SystemLP class which coordinates the formulation of power system
 * planning as a linear programming problem. Manages collections of system
 * components and their LP representations.
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
#include <gtopt/solver_options.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{
class LinearInterface;

/**
 * @concept AddToLP
 * @brief Concept for types that can be added to an LP problem
 *
 * Requires types to provide:
 * - add_to_lp() method for LP formulation
 * - add_to_output() method for result output
 */

template<typename T>
concept AddToLP = requires(T obj,
                           SystemContext& system_context,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           OutputContext& output_context) {
  { obj.add_to_lp(system_context, scenario, stage, lp) } -> std::same_as<bool>;
  { obj.add_to_output(output_context) } -> std::same_as<bool>;
};

// Verify all required types satisfy AddToLP concept
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
 * @brief Central coordinator for power system LP formulation
 *
 * Manages conversion of System components to LP representations and
 * coordinates:
 * - Creation of variables and constraints
 * - Time structure management (blocks, stages, scenarios)
 * - Grid component interactions
 * - Result output
 */
class SystemLP
{
public:
  SystemLP() = delete;
  SystemLP(const SystemLP&) = delete;
  SystemLP& operator=(const SystemLP&) noexcept = delete;

  SystemLP(SystemLP&&) noexcept = default;
  SystemLP& operator=(SystemLP&&) noexcept = default;
  ~SystemLP() noexcept = default;

  /**
   * @brief Construct from System with simulation and options
   * @param system The power system to model
   * @param simulation Simulation parameters
   * @param flat_opts Additional options (default empty)
   */
  explicit SystemLP(const System& system,
                    SimulationLP& simulation,
                    PhaseLP phase,
                    SceneLP scene,
                    const FlatOptions& flat_opts = {});

  explicit SystemLP(const System& system,
                    SimulationLP& simulation,
                    const FlatOptions& flat_opts = {})
      : SystemLP(system,
                 simulation,
                 PhaseLP {Phase {},
                          simulation.options(),
                          simulation.simulation(),
                          PhaseIndex {0}},
                 SceneLP {Scene {}, simulation.simulation(), SceneIndex {0}},
                 flat_opts)
  {
  }

  /// Tuple of collections for all LP component types
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

  template<typename Self>
  [[nodiscard]] constexpr auto&& collections(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_collections_;
  }

  /**
   * @brief Access the underlying system
   * @return Reference to the system
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& system(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_system_.get();
  }

  /**
   * @brief Get system context
   * @return Const reference to system context
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& system_context(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_system_context_;
  }

  /**
   * @brief Access linear interfaces
   * @return Linear interfaces container
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& linear_interface(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_linear_interface_;
  }

  /**
   * @brief Get system options
   * @return Const reference to options
   */
  [[nodiscard]] constexpr const auto& options() const noexcept
  {
    return system_context().options();
  }

  /**
   * @brief Add element to appropriate collection
   * @tparam Element Type of element to add
   * @param e Element to add
   * @return Result of collection push_back
   */
  template<typename Element>
  constexpr auto push_back(Element&& e) noexcept(noexcept(
      std::declval<Collection<Element>>().push_back(std::forward<Element>(e))))
  {
    return std::get<Collection<Element>>(m_collections_)
        .push_back(std::forward<Element>(e));
  }

  /**
   * @brief Get all elements of specific type
   * @tparam Element Type of elements to retrieve
   * @return Reference to elements container
   */
  template<typename Element, typename Self>
  [[nodiscard]] constexpr auto&& elements(this Self&& self) noexcept
  {
    return std::get<Collection<Element>>(
               std::forward<Self>(self).m_collections_)
        .elements();
  }

  /**
   * @brief Get element by ID
   * @tparam Element Type of element
   * @tparam Id ID type template
   * @param id Element ID
   * @return Reference to the element
   */
  template<typename Element, typename Self, template<typename> class Id>
  [[nodiscard]] constexpr auto&& element(this Self&& self,
                                         const Id<Element>& id) noexcept
  {
    return std::get<Collection<Element>>(
               std::forward<Self>(self).m_collections_)
        .element(id);
  }

  /**
   * @brief Get index of element by ID
   * @tparam Element Type of element
   * @tparam Id ID type template
   * @param id Element ID
   * @return Index of the element
   */
  template<typename Element, template<typename> class Id>
  [[nodiscard]] constexpr auto element_index(
      const Id<Element>& id) const noexcept
  {
    return std::get<Collection<Element>>(m_collections_).element_index(id);
  }

  template<typename Element, template<typename> class Id>
  [[nodiscard]] constexpr bool contains(const Id<Element>& id) const noexcept
  {
    return std::get<Collection<Element>>(m_collections_).contains(id);
  }

  /**
   * @brief Get system name
   * @return System name as string view
   */
  [[nodiscard]] constexpr NameView name() const noexcept
  {
    return system().name;
  }

  void create_lp(const FlatOptions& flat_opts = {});

  /**
   * @brief Write LP formulation to file
   * @param filename Output file path
   */
  void write_lp(const std::string& filename) const;

  /**
   * @brief Resolves the linear programming problem
   * @param solver_options Configuration options for the LP solver
   * @return Expected with solver status code (0 = optimal) or error
   */
  [[nodiscard]] std::expected<int, Error> resolve(
      const SolverOptions& solver_options = {});

  /**
   * @brief Write output for all linear interfaces
   */
  void write_out() const;

  [[nodiscard]] constexpr const auto& phase() const noexcept
  {
    return m_phase_;
  }

  [[nodiscard]] constexpr const auto& scene() const noexcept
  {
    return m_scene_;
  }

  [[nodiscard]] constexpr const auto& single_bus_id() const noexcept
  {
    return m_single_bus_id_;
  }

  template<typename Id>
  constexpr bool is_single_bus(const Id& id) const noexcept
  {
    if (m_single_bus_id_) {
      auto&& sid = *m_single_bus_id_;
      return sid.index() == 0 ? std::get<0>(sid) == id.first
                              : std::get<1>(sid) == id.second;
    }
    return false;
  }

private:
  std::reference_wrapper<const System> m_system_;
  SystemContext m_system_context_;
  collections_t m_collections_;
  PhaseLP m_phase_;
  SceneLP m_scene_;
  LinearInterface m_linear_interface_;
  std::optional<ObjectSingleId<BusLP>> m_single_bus_id_ {};
};

}  // namespace gtopt
