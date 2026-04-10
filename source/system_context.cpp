/**
 * @file      system_context.cpp
 * @brief     Implementation of SystemContext initialization
 * @date      Thu Jun 19 11:13:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the SystemContext constructor, which wires together
 * the simulation, system LP, cost helpers, and element collection pointers.
 */

#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

}  // namespace

namespace gtopt
{

SystemContext::SystemContext(SimulationLP& simulation, SystemLP& system)
    : FlatHelper(simulation)
    , CostHelper(
          simulation.options(), simulation.scenarios(), simulation.stages())
    , m_simulation_(simulation)
    , m_system_(system)
{
  // Populate m_collection_ptrs_: one void* per LP element type.
  // std::apply decomposes the collections tuple, and the C++20 template
  // lambda matches each Collection<Ts> by type.  lp_type_index_v<Ts> maps
  // each element type to its slot in m_collection_ptrs_, so the ordering in
  // SystemLP::collections_t need not match lp_element_types_t.
  // The fold `((ptr[i] = &coll), ...)` stores each collection's address into
  // the compile-time-indexed slot; the parens ensure sequenced evaluation.
  std::apply([this]<typename... Ts>(const Collection<Ts>&... colls) noexcept
             { ((m_collection_ptrs_[lp_type_index_v<Ts>] = &colls), ...); },
             system.collections());
}

auto SystemContext::get_bus_index(const ObjectSingleId<BusLP>& id) const
    -> ElementIndex<BusLP>
{
  return system().element_index(system().single_bus_id().value_or(id));
}

auto SystemContext::get_bus(const ObjectSingleId<BusLP>& id) const
    -> const BusLP&
{
  try {
    return system().element(get_bus_index(id));
  } catch (const std::out_of_range& e) {
    SPDLOG_ERROR(
        std::format("Bus with ID {} not found: {}", id.uid(), e.what()));
    throw;
  }
}

// ── PAMPL forwarders that need the full SystemLP definition ─────────────
//
// These can't live in the header because reaching `system().scene()` /
// `system().phase()` requires `system_lp.hpp`, which would create a
// circular include with `system_context.hpp`.

void SystemContext::add_ampl_variable(
    std::string_view class_name,
    Uid element_uid,
    std::string_view attribute,
    const ScenarioLP& scenario,
    const StageLP& stage,
    const BIndexHolder<ColIndex>& block_cols) const
{
  m_simulation_.get().add_ampl_variable(system().scene().index(),
                                        system().phase().index(),
                                        class_name,
                                        element_uid,
                                        attribute,
                                        scenario.uid(),
                                        stage.uid(),
                                        block_cols);
}

void SystemContext::add_ampl_variable(std::string_view class_name,
                                      Uid element_uid,
                                      std::string_view attribute,
                                      const ScenarioLP& scenario,
                                      const StageLP& stage,
                                      ColIndex stage_col) const
{
  m_simulation_.get().add_ampl_variable(system().scene().index(),
                                        system().phase().index(),
                                        class_name,
                                        element_uid,
                                        attribute,
                                        scenario.uid(),
                                        stage.uid(),
                                        stage_col);
}

std::optional<ColIndex> SystemContext::find_ampl_col(
    std::string_view class_name,
    Uid element_uid,
    std::string_view attribute,
    ScenarioUid scenario_uid,
    StageUid stage_uid,
    BlockUid block_uid) const
{
  return simulation().find_ampl_col(system().scene().index(),
                                    system().phase().index(),
                                    class_name,
                                    element_uid,
                                    attribute,
                                    scenario_uid,
                                    stage_uid,
                                    block_uid);
}

void SystemContext::register_ampl_element_metadata(
    std::string_view class_name,
    Uid element_uid,
    AmplElementMetadata metadata) const
{
  m_simulation_.get().register_ampl_element_metadata(system().scene().index(),
                                                     system().phase().index(),
                                                     class_name,
                                                     element_uid,
                                                     std::move(metadata));
}

const AmplElementMetadata* SystemContext::find_ampl_element_metadata(
    std::string_view class_name, Uid element_uid) const noexcept
{
  return simulation().find_ampl_element_metadata(system().scene().index(),
                                                 system().phase().index(),
                                                 class_name,
                                                 element_uid);
}

void SystemContext::defer_state_link(StateVariable::Key prev_key,
                                     ColIndex here_col) const
{
  m_system_.get().defer_state_link(prev_key,
                                   LPKey {
                                       .scene_index = system().scene().index(),
                                       .phase_index = system().phase().index(),
                                   },
                                   here_col);
}

}  // namespace gtopt
