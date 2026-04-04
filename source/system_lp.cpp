/**
 * @file      system_lp.cpp
 * @brief     Implementation of SystemLP class for power system LP formulation
 * @date      Tue Apr  8 01:20:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the SystemLP class, which is responsible for creating
 * and managing the linear programming formulation of power system planning
 * problems. It handles conversion of system components to their LP
 * representations, coordinates constraint generation across the system, and
 * provides utilities for adding constraints to the linear problem and
 * extracting results.
 */

#include <format>
#include <unordered_map>

#include <gtopt/bus_island.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

#include "gtopt/options_lp.hpp"
#include "gtopt/simulation_lp.hpp"
#include "gtopt/system_context.hpp"

namespace
{
using namespace gtopt;

/**
 * @brief Creates a collection of LP elements from system elements
 *
 * This function transforms a vector of system elements into a collection
 * of their corresponding LP representations.
 *
 * @tparam Out Output LP element type
 * @tparam Inp Input system element type
 * @tparam InputContext Type of input context
 * @param input_context Context for input processing
 * @param input Vector of system elements to transform
 * @return Collection of LP elements
 */
template<typename Out, typename Inp, typename InputContext>
auto make_collection(InputContext& input_context, const std::vector<Inp>& input)
    -> Collection<Out>
{
  return Collection<Out> {
      std::ranges::to<std::vector<Out>>(
          input
          | std::ranges::views::transform(
              [&](const auto& element)
              { return Out {element, input_context}; })),
  };
}

/**
 * @brief Creates a collection of LP elements from optional system elements
 *
 * Overload for optional vector input that handles the case when the input
 * vector might not be present.
 *
 * @tparam Out Output LP element type
 * @tparam Inp Input system element type
 * @tparam InputContext Type of input context
 * @param input_context Context for input processing
 * @param input Optional vector of system elements
 * @return Collection of LP elements or empty collection if input is empty
 */
template<typename Out, typename Inp, typename InputContext>
auto make_collection(InputContext& input_context,
                     const std::optional<std::vector<Inp>>& input)
    -> Collection<Out>
{
  if (input) [[likely]] {
    return make_collection<Out>(input_context, *input);
  }
  return Collection<Out> {};
}

//
//
//

constexpr auto add_to_lp(auto& collections,
                         SystemContext& system_context,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         LinearProblem& lp)
{
  const bool use_single_bus = system_context.options().use_single_bus();

  auto visitor = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, BusLP>) {
      return !use_single_bus || system_context.system().is_single_bus(e.id())
          ? e.add_to_lp(system_context, scenario, stage, lp)
          : true;
    } else if constexpr (std::is_same_v<T, LineLP>) {
      return !use_single_bus ? e.add_to_lp(system_context, scenario, stage, lp)
                             : true;
    } else {
      try {
        // For all other elements, just call their add_to_lp method
        return e.add_to_lp(system_context, scenario, stage, lp);
      } catch (const std::exception& ex) {
        SPDLOG_ERROR(
            std::format("Error adding {} uid={} to LP "
                        "(scenario={}, stage={}): {}",
                        T::ClassName,
                        e.uid(),
                        scenario.uid(),
                        stage.uid(),
                        ex.what()));
        return false;
      }
    }
  };

  auto count = visit_elements(collections, visitor);
  if (count == 0) [[unlikely]] {
    SPDLOG_WARN(
        std::format("No active elements found for scenario {} in stage {}",
                    scenario.uid(),
                    stage.uid()));
  }

  return count;
}

/// @brief Pin orphaned theta variables in disconnected bus islands.
///
/// After all elements are added to the LP for a (scenario, stage), some
/// buses may have theta columns that are not connected to any reference
/// bus through active Kirchhoff rows.  This happens when a line becomes
/// inactive at a particular stage, splitting the network.
///
/// For each connected component that lacks a reference bus we pin the
/// first theta to zero, preventing free-floating angles and unreliable
/// LMPs.
void fix_stage_islands(const auto& collections,
                       const ScenarioLP& scenario,
                       const StageLP& stage,
                       LinearProblem& lp)
{
  const auto& bus_coll = std::get<Collection<BusLP>>(collections);
  const auto& buses = bus_coll.elements();
  const auto& lines = std::get<Collection<LineLP>>(collections).elements();
  const auto n_buses = buses.size();
  if (n_buses <= 1 || stage.blocks().empty()) {
    return;
  }

  const auto first_buid = stage.blocks().front().uid();

  // Identify which buses have theta columns and which are references.
  // has_theta[i] is true when bus i created theta columns for this stage.
  std::vector<bool> has_theta(n_buses, false);
  std::vector<bool> is_reference(n_buses, false);
  std::size_t theta_count = 0;

  for (auto&& [idx, bus] : std::views::enumerate(buses)) {
    const auto i = static_cast<std::size_t>(idx);
    if (bus.lookup_theta_col(scenario, stage, first_buid).has_value()) {
      has_theta[i] = true;
      ++theta_count;
    }
    if (bus.reference_theta().has_value()) {
      is_reference[i] = true;
    }
  }

  // Nothing to check if no theta columns exist (single-bus or no Kirchhoff)
  if (theta_count <= 1) {
    return;
  }

  // Build DSU over buses connected by active lines with Kirchhoff rows.
  // Use Collection::element_index(SingleId) to resolve bus references
  // (handles both Uid and Name variants).
  DisjointSetUnion dsu(n_buses);
  const auto st_key = std::pair {scenario.uid(), stage.uid()};

  for (const auto& line : lines) {
    if (!line.is_active(stage) || line.is_loop()) {
      continue;
    }
    // Only lines that created Kirchhoff rows connect buses electrically
    if (!line.has_theta_rows(st_key)) {
      continue;
    }
    // Resolve bus_a/bus_b via the Collection's uid/name maps.
    try {
      const auto idx_a =
          static_cast<std::size_t>(bus_coll.element_index(line.bus_a_sid()));
      const auto idx_b =
          static_cast<std::size_t>(bus_coll.element_index(line.bus_b_sid()));
      if (!has_theta[idx_a] || !has_theta[idx_b]) {
        continue;
      }
      dsu.unite(idx_a, idx_b);
    } catch (const std::out_of_range&) {
      continue;  // Bus not found in collection — skip line
    }
  }

  // For each connected component of theta-bearing buses, check if it
  // contains a reference bus.  If not, pin the first bus's theta to zero.
  std::unordered_map<std::size_t, bool> root_has_ref;
  std::unordered_map<std::size_t, std::size_t> root_first_theta;

  for (std::size_t i = 0; i < n_buses; ++i) {
    if (!has_theta[i]) {
      continue;
    }
    const auto root = dsu.find(i);
    if (is_reference[i]) {
      root_has_ref[root] = true;
    }
    if (!root_first_theta.contains(root)) {
      root_first_theta[root] = i;
    }
  }

  for (const auto& [root, first_idx] : root_first_theta) {
    if (root_has_ref.contains(root)) {
      continue;  // This component already has a reference bus
    }

    // Pin the first bus's theta columns to zero for all blocks
    const auto& bus = buses[first_idx];

    for (const auto& block : stage.blocks()) {
      const auto theta_col = bus.lookup_theta_col(scenario, stage, block.uid());
      if (theta_col) {
        auto& col = lp.col_at(*theta_col);
        col.lowb = 0.0;
        col.uppb = 0.0;
      }
    }

    SPDLOG_WARN(
        "Stage {}: bus uid={} pinned as runtime reference "
        "(theta=0) for disconnected island",
        stage.uid(),
        bus.uid());
  }
}

constexpr auto create_linear_interface(auto& collections,
                                       SystemContext& system_context,
                                       const PhaseLP& phase,
                                       const SceneLP& scene,
                                       const auto& flat_opts)
{
  // Use scene/phase UIDs in the problem name so that CoinLpIO does not
  // warn about "missing objective function name" when writing .lp files.
  // Create the solver interface first so we can query its infinity value.
  LinearInterface li(flat_opts.solver_name);
  li.set_lp_names_level(static_cast<int>(flat_opts.lp_names_level));

  LinearProblem lp(std::format("gtopt_s{}_p{}", scene.uid(), phase.uid()));

  // Set the target infinity from the solver backend so that add_col/add_row
  // normalize DblMax bounds before flattening, avoiding solver warnings.
  lp.set_infinity(li.infinity());

  // Pre-reserve capacity to avoid repeated reallocations during build.
  // Each element typically adds ~2 cols and ~2 rows per block per scenario.
  {
    const auto n_elements = count_all_elements(collections);
    size_t total_blocks = 0;
    for (auto&& stage : phase.stages()) {
      total_blocks += stage.blocks().size();
    }
    const auto n_scenarios = scene.scenarios().size();
    constexpr size_t cols_per_element = 3;
    constexpr size_t rows_per_element = 2;
    const auto est_cols =
        n_elements * total_blocks * n_scenarios * cols_per_element;
    const auto est_rows =
        n_elements * total_blocks * n_scenarios * rows_per_element;
    lp.reserve(est_cols, est_rows);
  }

  const bool check_islands = !system_context.options().use_single_bus()
      && system_context.options().use_kirchhoff();

  // Process all active stages in phase
  for (auto&& stage : phase.stages()) {
    // Process all active scenarios in simulation
    for (auto&& scenario : scene.scenarios()) {
      add_to_lp(collections, system_context, scenario, stage, lp);

      // After all elements are added for this (scenario, stage), check
      // for disconnected bus islands created by inactive lines and pin
      // an orphaned theta variable as a runtime reference if needed.
      if (check_islands) {
        fix_stage_islands(collections, scenario, stage, lp);
      }
    }
  }

  // Convert and store the flattened LP representation
  auto flat_lp = lp.flatten(flat_opts);
  li.load_flat(flat_lp);
  return li;
}

void create_collections(const auto& system_context,
                        const auto& sys,
                        SystemLP::collections_t& colls)
{
  // NOTE: colls is system_lp.m_collections_, already default-constructed
  // (valid but empty) before this function is called.  Each collection must
  // be assigned before any later collection whose constructor looks it up via
  // InputContext::element_index — that path goes back into
  // system_lp.m_collections_ (i.e., colls), so the earlier entries must already
  // be present.
  InputContext ic(system_context);

  std::get<Collection<BusLP>>(colls) =
      make_collection<BusLP>(ic, sys.bus_array);
  std::get<Collection<DemandLP>>(colls) =
      make_collection<DemandLP>(ic, sys.demand_array);
  std::get<Collection<GeneratorLP>>(colls) =
      make_collection<GeneratorLP>(ic, sys.generator_array);
  std::get<Collection<LineLP>>(colls) =
      make_collection<LineLP>(ic, sys.line_array);
  std::get<Collection<GeneratorProfileLP>>(colls) =
      make_collection<GeneratorProfileLP>(ic, sys.generator_profile_array);
  std::get<Collection<DemandProfileLP>>(colls) =
      make_collection<DemandProfileLP>(ic, sys.demand_profile_array);
  std::get<Collection<BatteryLP>>(colls) =
      make_collection<BatteryLP>(ic, sys.battery_array);
  std::get<Collection<ConverterLP>>(colls) =
      make_collection<ConverterLP>(ic, sys.converter_array);
  std::get<Collection<ReserveZoneLP>>(colls) =
      make_collection<ReserveZoneLP>(ic, sys.reserve_zone_array);
  std::get<Collection<ReserveProvisionLP>>(colls) =
      make_collection<ReserveProvisionLP>(ic, sys.reserve_provision_array);

  std::get<Collection<JunctionLP>>(colls) =
      make_collection<JunctionLP>(ic, sys.junction_array);
  std::get<Collection<WaterwayLP>>(colls) =
      make_collection<WaterwayLP>(ic, sys.waterway_array);
  std::get<Collection<FlowLP>>(colls) =
      make_collection<FlowLP>(ic, sys.flow_array);
  std::get<Collection<ReservoirLP>>(colls) =
      make_collection<ReservoirLP>(ic, sys.reservoir_array);
  std::get<Collection<ReservoirSeepageLP>>(colls) =
      make_collection<ReservoirSeepageLP>(ic, sys.reservoir_seepage_array);
  std::get<Collection<ReservoirDischargeLimitLP>>(colls) =
      make_collection<ReservoirDischargeLimitLP>(
          ic, sys.reservoir_discharge_limit_array);
  std::get<Collection<TurbineLP>>(colls) =
      make_collection<TurbineLP>(ic, sys.turbine_array);
  std::get<Collection<ReservoirProductionFactorLP>>(colls) =
      make_collection<ReservoirProductionFactorLP>(
          ic, sys.reservoir_production_factor_array);

  // Water rights (NOT part of hydro topology)
  std::get<Collection<FlowRightLP>>(colls) =
      make_collection<FlowRightLP>(ic, sys.flow_right_array);
  std::get<Collection<VolumeRightLP>>(colls) =
      make_collection<VolumeRightLP>(ic, sys.volume_right_array);

  // UserConstraintLP is placed LAST so that user-constraint rows are added to
  // the LP after all other elements whose columns they reference.
  std::get<Collection<UserConstraintLP>>(colls) =
      make_collection<UserConstraintLP>(ic, sys.user_constraint_array);

#ifdef GTOPT_EXTRA
  std::get<Collection<EmissionZoneLP>>(colls) =
      make_collection<EmissionZoneLP>(ic, sys.emission_zones);
  std::get<Collection<GeneratorEmissionLP>>(colls) =
      make_collection<GeneratorEmissionLP>(ic, sys.generator_emissions);
  std::get<Collection<DemandEmissionLP>>(colls) =
      make_collection<DemandEmissionLP>(ic, sys.demand_emissions);
#endif
}

}  // namespace

namespace gtopt
{
void SystemLP::create_lp(const LpMatrixOptions& flat_opts)
{
  m_linear_interface_ = create_linear_interface(
      collections(), system_context(), phase(), scene(), flat_opts);
}

SystemLP::SystemLP(const System& system,
                   SimulationLP& simulation,
                   PhaseLP phase,
                   SceneLP scene,
                   const LpMatrixOptions& flat_opts)
    : m_system_(system)
    , m_system_context_(simulation, *this)
    , m_phase_(std::move(phase))
    , m_scene_(std::move(scene))
{
  // m_collections_ is default-constructed (valid, empty) before this point.
  // Populate it in-place so that each sub-collection is visible to
  // InputContext::element_index as soon as it is built, allowing later
  // collections (e.g. ReserveProvisionLP) to look up earlier ones
  // (e.g. GeneratorLP) without accessing uninitialized memory.
  create_collections(m_system_context_, system, m_collections_);

  if (options().use_single_bus()) {
    const auto& buses = system.bus_array;
    if (!buses.empty()) {
      m_single_bus_id_.emplace(buses.front().uid);
    }
  }

  create_lp(flat_opts);
}

void SystemLP::write_out() const
{
  OutputContext oc(
      system_context(), linear_interface(), scene().uid(), phase().uid());

  auto count = visit_elements(
      collections(), [&oc](const auto& e) { return e.add_to_output(oc); });

  if (count <= 0) {
    SPDLOG_WARN("No elements added to output");
    return;
  }

  oc.write();
}

auto SystemLP::write_lp(const std::string& filename) const
    -> std::expected<std::string, Error>
{
  // Use UIDs (always valid: default Phase/Scene are assigned uid=0 in
  // simulation_lp.cpp when phase_array/scene_array are empty).
  // Naming convention: {stem}_scene_{scene_uid}_phase_{phase_uid}
  const auto fname =
      as_label(filename, "scene", scene().uid(), "phase", phase().uid());

  auto result = linear_interface().write_lp(fname);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  return fname + ".lp";
}

std::expected<int, Error> SystemLP::resolve(const SolverOptions& solver_options)
{
  return linear_interface().resolve(solver_options);
}

int SystemLP::update_lp()
{
  if (!linear_interface().supports_set_coeff()) {
    return 0;
  }

  int total = 0;

  for (auto&& scenario : scene().scenarios()) {
    for (auto&& stage : phase().stages()) {
      visit_elements(collections(),
                     [&total, this, &scenario, &stage](auto& element) -> bool
                     {
                       using T = std::decay_t<decltype(element)>;
                       if constexpr (HasUpdateLP<T>) {
                         total += element.update_lp(*this, scenario, stage);
                       }
                       return true;
                     });
    }
  }

  return total;
}

}  // namespace gtopt
