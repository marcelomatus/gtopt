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

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <ranges>
#include <unordered_map>

#include <gtopt/bus_island.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_fingerprint.hpp>
#include <gtopt/map_reserve.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

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
        // User-constraint failures propagate in strict/debug mode so the
        // caller can surface non-convex rejections, unknown parameters,
        // and similar author errors instead of silently dropping the
        // constraint.  Other element types keep the original "log and
        // continue" behavior for most failures, but `std::runtime_error`
        // is reserved for fail-fast schema/author errors (missing
        // `stage.month` on a `reset_month` VR, monthly parameter out of
        // range, etc.) and always propagates so the caller cannot
        // silently produce a wrong LP.
        if constexpr (std::is_same_v<T, UserConstraintLP>) {
          const auto mode = system_context.options().constraint_mode();
          if (mode == ConstraintMode::strict || mode == ConstraintMode::debug) {
            throw;
          }
        }
        if (dynamic_cast<const std::runtime_error*>(&ex) != nullptr) {
          throw;
        }
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

  for (const auto& [i, bus] : enumerate(buses)) {
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
  const auto st_key = std::tuple {scenario.uid(), stage.uid()};

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
  map_reserve(root_has_ref, n_buses);
  map_reserve(root_first_theta, n_buses);

  for (const auto i : iota_range(std::size_t {0}, n_buses)) {
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

/// @brief Resolve a stage-indexed OptTRealFieldSched to a scalar value.
///
/// Handles the three cases: scalar → return directly, vector → index by stage
/// ordinal index (dense position), FileSched → unsupported (returns 0).
double resolve_stage_field(const OptTRealFieldSched& field,
                           StageIndex stage_index)
{
  if (!field.has_value()) {
    return 0.0;
  }
  const auto& val = *field;
  if (std::holds_alternative<Real>(val)) {
    return std::get<Real>(val);
  }
  if (std::holds_alternative<std::vector<Real>>(val)) {
    const auto& vec = std::get<std::vector<Real>>(val);
    const auto sidx = static_cast<size_t>(stage_index);
    if (sidx < vec.size()) {
      return vec[sidx];
    }
  }
  return 0.0;
}

/// @brief Add emission cap constraint for a (scenario, stage) pair.
///
/// If the system options define an emission_cap for this stage, adds a single
/// constraint row:
///   sum_g sum_b (emission_factor_g × duration_b × p_{g,b}) <= emission_cap_s
///
/// This aggregates across all generators that have a non-zero emission factor.
///
/// @note For generators with piecewise heat rate segments and per-segment
/// fuel_emission_factor, this uses the flat generator emission_factor on the
/// total generation variable p.  A more accurate formulation would use
/// per-segment emission coefficients (fuel_emission_factor × heat_rate_k)
/// on each segment variable δ_k, but that requires cross-collection access
/// to CommitmentLP segment columns.  TODO: refine when segment-level emission
/// accounting is needed for emission-cap-binding scenarios.
void add_emission_cap(const auto& collections,
                      SystemContext& system_context,
                      const ScenarioLP& scenario,
                      const StageLP& stage,
                      LinearProblem& lp)
{
  const auto stage_cap = resolve_stage_field(
      system_context.options().emission_cap(), stage.index());
  if (stage_cap <= 0.0) {
    return;
  }

  const auto& generators =
      std::get<Collection<GeneratorLP>>(collections).elements();

  auto row =
      SparseRow {
          .class_name = "System",
          .constraint_name = "emission_cap",
          .context = make_stage_context(scenario.uid(), stage.uid()),
      }
          .less_equal(stage_cap);

  bool has_terms = false;

  for (const auto& gen : generators) {
    if (!gen.is_active(stage)) {
      continue;
    }
    const auto ef = gen.param_emission_factor(stage.uid()).value_or(0.0);
    if (ef <= 0.0) {
      continue;
    }

    const auto& gen_cols = gen.generation_cols_at(scenario, stage);
    for (const auto& block : stage.blocks()) {
      const auto it = gen_cols.find(block.uid());
      if (it == gen_cols.end()) {
        continue;
      }
      const auto coeff = ef * block.duration();
      row[it->second] = coeff;
      has_terms = true;
    }
  }

  if (has_terms) {
    std::ignore = lp.add_row(std::move(row));
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

  // Build the LabelMaker from the naming bools and install it on both the
  // LinearProblem (used during flatten() for colnm/rownm) and the
  // LinearInterface (used for labels on any rows/cols added after
  // load_flat()).  The LabelMaker travels by value through
  // FlatLinearProblem::label_maker during load_flat().
  const auto eff_level =
      (flat_opts.row_with_name_map || flat_opts.col_with_names)
      ? LpNamesLevel::all
      : LpNamesLevel::none;
  const LabelMaker label_maker {eff_level};
  li.set_label_maker(label_maker);

  LinearProblem lp(std::format("gtopt_s{}_p{}", scene.uid(), phase.uid()));
  lp.set_label_maker(label_maker);

  // Set the target infinity from the solver backend so that add_col/add_row
  // normalize DblMax bounds before flattening, avoiding solver warnings.
  lp.set_infinity(li.infinity());

  // Set the VariableScaleMap so add_col() auto-resolves scales from metadata.
  lp.set_variable_scale_map(system_context.options().variable_scale_map());

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

      // Add system-wide emission cap constraint if configured
      add_emission_cap(collections, system_context, scenario, stage, lp);
    }
  }

  // Compute LP fingerprint before flattening (structured metadata is still
  // available in the raw cols/rows vectors).  Skipped unless the user
  // requested fingerprint output (--lp-fingerprint).
  auto fingerprint = flat_opts.compute_fingerprint
      ? compute_lp_fingerprint(lp.get_cols(), lp.get_rows())
      : LpFingerprint {};

  // Convert and store the flattened LP representation
  auto flat_lp = lp.flatten(flat_opts);

  // Branch on low_memory_mode:
  //
  //  * `off` (default): eagerly load the backend and stash the flat LP as
  //    an inert snapshot — current behavior.
  //
  //  * `snapshot` / `compress`: skip the initial `load_flat()` entirely.
  //    The snapshot is installed via `defer_initial_load`, which marks
  //    the backend as released so the first user-driven access goes
  //    through `ensure_backend()` → `reconstruct_backend()` → a single
  //    `load_flat()`.  Saves one full backend population per
  //    (scene, phase) under SDDP / cascade.
  if (flat_opts.low_memory_mode != LowMemoryMode::off) {
    li.set_low_memory(flat_opts.low_memory_mode,
                      select_codec(flat_opts.memory_codec),
                      /*cache_warm_start=*/false);
    li.defer_initial_load(std::move(flat_lp));
  } else {
    li.load_flat(flat_lp);
    li.save_snapshot(std::move(flat_lp));
  }
  return std::tuple {std::move(li), std::move(fingerprint)};
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
  std::get<Collection<CommitmentLP>>(colls) =
      make_collection<CommitmentLP>(ic, sys.commitment_array);
  std::get<Collection<SimpleCommitmentLP>>(colls) =
      make_collection<SimpleCommitmentLP>(ic, sys.simple_commitment_array);
  std::get<Collection<InertiaZoneLP>>(colls) =
      make_collection<InertiaZoneLP>(ic, sys.inertia_zone_array);
  std::get<Collection<InertiaProvisionLP>>(colls) =
      make_collection<InertiaProvisionLP>(ic, sys.inertia_provision_array);

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
  std::get<Collection<PumpLP>>(colls) =
      make_collection<PumpLP>(ic, sys.pump_array);
  std::get<Collection<ReservoirProductionFactorLP>>(colls) =
      make_collection<ReservoirProductionFactorLP>(
          ic, sys.reservoir_production_factor_array);

  // Water rights (NOT part of hydro topology)
  std::get<Collection<FlowRightLP>>(colls) =
      make_collection<FlowRightLP>(ic, sys.flow_right_array);
  std::get<Collection<VolumeRightLP>>(colls) =
      make_collection<VolumeRightLP>(ic, sys.volume_right_array);

  // Fuel storage
  std::get<Collection<LngTerminalLP>>(colls) =
      make_collection<LngTerminalLP>(ic, sys.lng_terminal_array);

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

/// Hoisted AMPL element-name + compound registration.
///
/// Element names and class-level compound recipes are
/// (scene, phase)-independent: each element has the same name in every
/// LP, and `line.flow = +flowp − flown` is the same recipe everywhere.
/// Populating these registries from inside the per-element `add_to_lp`
/// would require a mutex on the parallel scene-build loop, even though
/// every scene would write the identical entries.
///
/// Instead this helper runs once per `SimulationLP` via `std::call_once`
/// from the `SystemLP` constructor (see below).  That gives the registry
/// a single deterministic populate step regardless of whether the
/// `SystemLP` is built through `PlanningLP::create_systems` (parallel
/// across scenes) or directly by tests that construct one `SimulationLP`
/// + `SystemLP` pair.
template<typename LP, typename Array>
void register_element_names(SimulationLP& sim, const Array& arr)
{
  constexpr auto class_name = LP::ClassName.snake_case();
  for (const auto& obj : arr) {
    sim.register_ampl_element(class_name, obj.name, obj.uid);
  }
}

void register_all_ampl_element_names(SimulationLP& sim, const System& sys)
{
  register_element_names<BatteryLP>(sim, sys.battery_array);
  register_element_names<BusLP>(sim, sys.bus_array);
  register_element_names<ConverterLP>(sim, sys.converter_array);
  register_element_names<DemandLP>(sim, sys.demand_array);
  register_element_names<FlowLP>(sim, sys.flow_array);
  register_element_names<FlowRightLP>(sim, sys.flow_right_array);
  register_element_names<GeneratorLP>(sim, sys.generator_array);
  register_element_names<JunctionLP>(sim, sys.junction_array);
  register_element_names<LineLP>(sim, sys.line_array);
  register_element_names<ReserveProvisionLP>(sim, sys.reserve_provision_array);
  register_element_names<ReserveZoneLP>(sim, sys.reserve_zone_array);
  register_element_names<SimpleCommitmentLP>(sim, sys.simple_commitment_array);
  register_element_names<InertiaZoneLP>(sim, sys.inertia_zone_array);
  register_element_names<InertiaProvisionLP>(sim, sys.inertia_provision_array);
  register_element_names<ReservoirLP>(sim, sys.reservoir_array);
  register_element_names<TurbineLP>(sim, sys.turbine_array);
  register_element_names<PumpLP>(sim, sys.pump_array);
  register_element_names<VolumeRightLP>(sim, sys.volume_right_array);
  register_element_names<WaterwayLP>(sim, sys.waterway_array);
  register_element_names<LngTerminalLP>(sim, sys.lng_terminal_array);

  // Intentional exception: ReservoirSeepageLP is exposed at the AMPL
  // level under "seepage", not the snake-case of its class name
  // ("reservoir_seepage").  Mirrors the constraint/variable name
  // emitted by `source/reservoir_seepage_lp.cpp`.
  {
    constexpr auto class_name = ReservoirSeepageLP::SeepageName;
    for (const auto& obj : sys.reservoir_seepage_array) {
      sim.register_ampl_element(class_name, obj.name, obj.uid);
    }
  }

  // Class-level compound: `line.flow = +flowp − flown`.
  // Registered once globally; the resolver expands it per-(uid, block).
  {
    constexpr auto line_class = LineLP::ClassName.snake_case();
    sim.add_ampl_compound(line_class,
                          LineLP::FlowName,
                          {
                              AmplCompoundLeg {
                                  .coefficient = +1.0,
                                  .source_attribute = LineLP::FlowpName,
                              },
                              AmplCompoundLeg {
                                  .coefficient = -1.0,
                                  .source_attribute = LineLP::FlownName,
                              },
                          });
  }

  // ── options.* scalar allow-list (Phase 1d) ────────────────────────────
  //
  // Explicit allow-list (not full-open): only fields that are intended
  // to be referenceable from PAMPL user-constraints are exposed.  Bools
  // are surfaced as 0.0 / 1.0 so the constraint DSL can use them as
  // ordinary numeric coefficients.  Cached by value at registration —
  // options are immutable for the SimulationLP lifetime.
  {
    static constexpr std::string_view options_class = "options";
    const auto& opts = sim.options();
    sim.add_ampl_scalar(
        options_class, "annual_discount_rate", opts.annual_discount_rate());
    sim.add_ampl_scalar(
        options_class, "scale_objective", opts.scale_objective());
    sim.add_ampl_scalar(options_class, "scale_theta", opts.scale_theta());
    sim.add_ampl_scalar(
        options_class, "kirchhoff_threshold", opts.kirchhoff_threshold());
    sim.add_ampl_scalar(
        options_class, "use_kirchhoff", opts.use_kirchhoff() ? 1.0 : 0.0);
    sim.add_ampl_scalar(
        options_class, "use_single_bus", opts.use_single_bus() ? 1.0 : 0.0);
    sim.add_ampl_scalar(
        options_class, "use_line_losses", opts.use_line_losses() ? 1.0 : 0.0);
  }

  // ── Mode-driven AMPL suppression (Tier 1 / Tier 2) ───────────────────
  //
  // Translate planning-option flags into explicit class/attribute
  // suppression entries so that user-constraint references to modes
  // that are turned off get silently dropped rather than throwing.
  //
  // Rationale: a modeller writing `line('l1').flow <= 100` should not
  // have to know whether a particular run uses `use_single_bus=true`.
  // The constraint is vacuously inapplicable in that mode and should
  // stay in the file without breaking the run.
  //
  // Typos (e.g. `lineee('l1').flow`, `line('l1').flowx`) are still
  // caught because those class/attribute strings are not in the
  // suppression map — the strict branch in
  // `user_constraint_lp.cpp` will still throw.
  {
    const auto& opts = sim.options();
    constexpr auto line_cls = LineLP::ClassName.snake_case();
    constexpr auto bus_cls = BusLP::ClassName.snake_case();

    // The two flags are independent:
    //   * !use_kirchhoff suppresses `bus.theta` (theta columns are not
    //     materialized without the Kirchhoff path).
    //   * use_single_bus suppresses the whole `line` class AND
    //     `bus.theta` (LineLP early-exits, Kirchhoff is moot).
    // When both apply, `use_single_bus=true` is the more fundamental
    // reason, so it is registered second to win insert_or_assign.
    if (!opts.use_kirchhoff()) {
      sim.suppress_ampl_attribute(
          bus_cls, BusLP::ThetaName, "use_kirchhoff=false");
    }
    if (opts.use_single_bus()) {
      sim.suppress_ampl_class(line_cls, "use_single_bus=true");
      sim.suppress_ampl_attribute(
          bus_cls, BusLP::ThetaName, "use_single_bus=true");
    }
  }

  // ── Per-element optional attributes ─────────────────────────────────
  //
  // `capainst`, `capacost`, `expmod` are created by
  // `CapacityObjectBase::add_to_lp` only for elements where the stage's
  // `expcap * expmod > 0` (expansion is configured) OR the previous
  // phase publishes a state-backed capainst/capacost.  In practice the
  // vast majority of generators/demands/lines/etc. have no expansion,
  // so these columns are **absent by design per-element**.
  //
  // A user writing `sum(g in generator(all), g.capainst) <= budget`
  // should not have to pre-filter for expanding generators — missing
  // columns should drop silently from the sum.  Declaring the
  // attributes as suppressed (with an "inherently optional" reason)
  // reuses the mode-driven resolver path and achieves this.
  //
  // Typo protection is coarser for these attributes than for fully
  // required ones: `generator('g1').capainst` on a non-expanding g1
  // drops silently rather than throwing.  This is the accepted
  // trade-off — `resolve_single_col` still emits an `SPDLOG_WARN` for
  // unknown element names, so element-name typos remain visible.
  {
    constexpr std::string_view reason =
        "per-element: capacity expansion is opt-in";
    for (const auto cls :
         {
             GeneratorLP::ClassName.snake_case(),
             DemandLP::ClassName.snake_case(),
             LineLP::ClassName.snake_case(),
             ConverterLP::ClassName.snake_case(),
             BatteryLP::ClassName.snake_case(),
         })
    {
      sim.suppress_ampl_attribute(
          cls, CapacityObjectBase::CapainstName, reason);
      sim.suppress_ampl_attribute(
          cls, CapacityObjectBase::CapacostName, reason);
      sim.suppress_ampl_attribute(cls, CapacityObjectBase::ExpmodName, reason);
    }
  }
}

}  // namespace

namespace gtopt
{
void SystemLP::create_lp(const LpMatrixOptions& flat_opts_in)
{
  // Inject scale_objective from planning options if not already set,
  // so that flatten() applies the global objective divisor.
  auto flat_opts = flat_opts_in;
  if (flat_opts.scale_objective == 1.0) {
    flat_opts.scale_objective = system_context().options().scale_objective();
  }
  // create_linear_interface owns the snapshot installation: it either
  // load_flats + save_snapshots eagerly (low_memory off) or installs the
  // flat LP as a deferred snapshot via defer_initial_load (otherwise).
  auto [li, fp] = create_linear_interface(
      collections(), system_context(), phase(), scene(), flat_opts);
  m_linear_interface_ = std::move(li);
  m_fingerprint_ = std::move(fp);
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
  // Enable the per-cell AMPL variable registry when user constraints
  // need to resolve element columns.  Without user constraints the
  // map stays empty, saving allocation/hashing overhead.
  if (!system.user_constraint_array.empty()) {
    simulation.set_need_ampl_variables(/*v=*/true);
  }

  // Populate the SimulationLP-wide AMPL element-name and compound
  // registries exactly once, before any per-(scene, phase) build runs.
  // The std::call_once flag is owned by SimulationLP, so this works
  // both under PlanningLP's parallel scene loop (only the first SystemLP
  // built actually does the work; the rest pass through) and for tests
  // that build a single SimulationLP/SystemLP pair directly.
  std::call_once(simulation.ampl_registry_flag(),
                 [&] { register_all_ampl_element_names(simulation, system); });

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

SystemLP::SystemLP(SystemLP&& other) noexcept
    : m_system_(other.m_system_)
    , m_system_context_(std::move(other.m_system_context_))
    , m_collections_(std::move(other.m_collections_))
    , m_phase_(std::move(other.m_phase_))
    , m_scene_(std::move(other.m_scene_))
    , m_linear_interface_(std::move(other.m_linear_interface_))
    , m_fingerprint_(std::move(other.m_fingerprint_))
    , m_single_bus_id_(std::move(other.m_single_bus_id_))
    , m_pending_state_links_(std::move(other.m_pending_state_links_))
    , m_prev_phase_sys_(other.m_prev_phase_sys_)
{
  // After member-wise move, m_system_context_ still holds a
  // reference_wrapper to the moved-from SystemLP and stale interior
  // pointers into the moved-from m_collections_ tuple.  Re-point both
  // to *this.
  m_system_context_.rebind_system(*this);
}

SystemLP& SystemLP::operator=(SystemLP&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  // Move-assign invariant: both ends must already refer to the same
  // underlying `System`.  `m_system_context_` holds a stable reference
  // (via its base helpers) into that System's metadata; rebind_system
  // below only re-points its back-reference to *this and rebuilds the
  // collection-pointer table — it does NOT rewire the System reference.
  // Cross-System move-assign would silently leave dangling state, so
  // enforce the invariant unconditionally.  `assert` is compiled out
  // under NDEBUG, so we use `std::terminate` to keep the check live in
  // release builds as well (noexcept-compatible).
  if (&m_system_.get() != &other.m_system_.get()) [[unlikely]] {
    std::terminate();
  }
  m_system_ = other.m_system_;
  m_system_context_ = std::move(other.m_system_context_);
  m_collections_ = std::move(other.m_collections_);
  m_phase_ = std::move(other.m_phase_);
  m_scene_ = std::move(other.m_scene_);
  m_linear_interface_ = std::move(other.m_linear_interface_);
  m_fingerprint_ = std::move(other.m_fingerprint_);
  m_single_bus_id_ = std::move(other.m_single_bus_id_);
  m_pending_state_links_ = std::move(other.m_pending_state_links_);
  m_prev_phase_sys_ = other.m_prev_phase_sys_;
  m_system_context_.rebind_system(*this);
  return *this;
}

void SystemLP::write_out()
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

  // Write LP fingerprint if requested
  if (options().lp_fingerprint()) {
    const auto fname = as_label(
        "lp_fingerprint", "scene", scene().uid(), "phase", phase().uid());
    const auto filepath = (std::filesystem::path(options().output_directory())
                           / (fname + ".json"))
                              .string();
    write_lp_fingerprint(fingerprint(), filepath, scene().uid(), phase().uid());
  }
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
