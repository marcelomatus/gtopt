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
#include <atomic>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <ranges>
#include <unordered_map>

#include <gtopt/ampl_dispatch_registry.hpp>
#include <gtopt/bus_island.hpp>
#include <gtopt/constraint_names.hpp>
#include <gtopt/gtopt_main.hpp>
#include <gtopt/kirchhoff_cycle_basis.hpp>
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

    // Passive parameter-carrier elements (Fuel, Emission Commit-1, …)
    // have no `add_to_lp` method by design — they exist in the
    // collection only so `SystemContext::element<T>(...)` can resolve
    // them.  Skip them at compile time.  The `HasAddToLp` capability
    // concept is the single source of truth for "this element type
    // contributes LP rows / cols / coefficients" — a discriminator, not
    // an enforced interface (output-only / planning-level elements are
    // skipped here and handled by their own pass).
    if constexpr (!HasAddToLp<T>) {
      return true;
    } else if constexpr (std::is_same_v<T, BusLP>) {
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
                        T::Element::class_name,
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
        // `std::logic_error` (and subclasses: `std::out_of_range`,
        // `std::invalid_argument`, `std::domain_error`, etc.) signals
        // a programmer/data bug — `flat_map::at` on a missing key, a
        // sentinel array indexed past its size, etc.  These are NOT
        // transient noise that "log and continue" can paper over: the
        // LP being built is structurally wrong and continuing produces
        // either a malformed solve or a downstream cascade of thousands
        // of identical errors that bury the root cause.  Fail fast so
        // the caller (gtopt_main) returns a clean non-zero exit with
        // one diagnostic on stderr instead of a 6k-error log file.
        if (dynamic_cast<const std::logic_error*>(&ex) != nullptr) {
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

/// @brief Planning-level (coarse-scope) build passes for one (scene, phase)
/// cell.  Runs once, AFTER the per-(scenario, stage) `add_to_lp` loop.
///
/// Two ordered sweeps over all collections:
///   1. phase scope  — recourse / per-phase state variables (piece 5);
///   2. global scope — the FutureCost α terminal cut + annual caps (piece 2),
/// so the global sweep can reference state columns the phase sweep just
/// registered.  Each element is dispatched via `if constexpr (HasAddTo*Lp<T>)`;
/// passive / per-block-only elements are skipped at compile time.  No current
/// element provides either method, so both sweeps are inert no-ops until
/// `FutureCostLP` / `UserModelLP` land — the `[[maybe_unused]]` markers keep
/// `-Werror` happy while every `if constexpr` branch is the (discarded) `else`.
///
/// @return total participating-element count across both sweeps.
constexpr auto add_to_planning_lp(
    auto& collections,
    [[maybe_unused]] SystemContext& system_context,
    [[maybe_unused]] const SceneLP& scene,
    [[maybe_unused]] const PhaseLP& phase,
    [[maybe_unused]] LinearProblem& lp)
{
  auto phase_visitor = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;
    if constexpr (HasAddToPhaseLp<T>) {
      return e.add_to_phase_lp(system_context, scene, phase, lp);
    } else {
      return true;
    }
  };
  auto global_visitor = [&](auto& e) -> bool
  {
    using T = std::decay_t<decltype(e)>;
    if constexpr (HasAddToGlobalLp<T>) {
      return e.add_to_global_lp(system_context, scene, phase, lp);
    } else {
      return true;
    }
  };

  const auto phase_count = visit_elements(collections, phase_visitor);
  const auto global_count = visit_elements(collections, global_visitor);
  return phase_count + global_count;
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

    // Per-stage runtime-island pin: emitted once per disconnected
    // component on every (scenario, stage) build, so a 50-stage / 16-
    // scene run produces hundreds of identical lines on cases with
    // any inactive lines.  Demoted to TRACE — visible only when the
    // user enables trace logging via `-T`/`--trace-log`.  Use the
    // function form (`spdlog::trace`) rather than `SPDLOG_TRACE` so
    // the call stays runtime-gated even though the PCH bakes spdlog
    // at INFO (memory: `feedback_spdlog_trace_macro.md`).
    spdlog::trace(
        "Stage {}: bus uid={} pinned as runtime reference "
        "(theta=0) for disconnected island",
        stage.uid(),
        bus.uid());
  }
}

/// Build the LinearProblem from collections + flatten it, returning the
/// flat LP, fingerprint, and the LabelMaker used.  Used by the eager
/// `create_linear_interface` path (one-shot build at construction).
/// @p solver_infinity is queried from the owning `LinearInterface`
/// (via `infinity()`) before the call so that DblMax bounds are
/// normalised to the correct value.
///
/// @p reserve_cols / @p reserve_rows override the shape heuristic when
/// non-zero.  SystemLP caches the actual (ncols, nrows) from the first
/// flatten and feeds them back in on every subsequent rebuild, avoiding
/// vector-growth reallocations that the heuristic tends to undershoot.
constexpr auto flatten_from_collections(auto& collections,
                                        SystemContext& system_context,
                                        const PhaseLP& phase,
                                        const SceneLP& scene,
                                        const auto& flat_opts,
                                        double solver_infinity,
                                        size_t reserve_cols = 0,
                                        size_t reserve_rows = 0)
{
  const auto eff_level =
      (flat_opts.row_with_name_map || flat_opts.col_with_names)
      ? LpNamesLevel::all
      : LpNamesLevel::none;
  const LabelMaker label_maker {eff_level};

  LinearProblem lp(as_label("gtopt", scene.uid(), phase.uid()));
  lp.set_label_maker(label_maker);
  lp.set_infinity(solver_infinity);
  lp.set_variable_scale_map(system_context.options().variable_scale_map());

  // Pre-reserve capacity.  On the first call we use a shape heuristic
  // (elements × blocks × scenarios × constants).  On subsequent rebuilds
  // the caller passes the exact counts from the last flatten, killing
  // reallocation overhead.
  if (reserve_cols > 0 || reserve_rows > 0) {
    lp.reserve(reserve_cols, reserve_rows);
  } else {
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

  const auto& sc_opts = system_context.options();
  const bool kirchhoff_active =
      !sc_opts.use_single_bus() && sc_opts.use_kirchhoff();
  const bool is_cycle_basis = kirchhoff_active
      && sc_opts.kirchhoff_mode() == KirchhoffMode::cycle_basis;
  // Theta-pin sweep is needed only in node_angle mode — cycle_basis
  // has no theta variables to pin.
  const bool check_islands = kirchhoff_active && !is_cycle_basis;

  // One-time per-cell INFO so operators can confirm which Kirchhoff
  // formulation actually fires.  Without this the JSON might say
  // ``kirchhoff_mode: "cycle_basis"`` while the cell silently fell
  // back to node_angle (e.g. when the cascade level filter dropped
  // the override) — an invisible failure mode for a flag whose only
  // payoff is LP-size reduction.
  if (kirchhoff_active) {
    SPDLOG_DEBUG("system_lp: kirchhoff_mode={} (cycle_basis={})",
                 enum_name(sc_opts.kirchhoff_mode()),
                 is_cycle_basis ? "yes" : "no");
  }

  // Process all active stages in phase
  for (auto&& stage : phase.stages()) {
    // Process all active scenarios in simulation
    for (auto&& scenario : scene.scenarios()) {
      add_to_lp(collections, system_context, scenario, stage, lp);

      // cycle_basis: emit the per-cycle KVL rows now that every
      // LineLP::add_to_lp has finished creating its flow vars.  The
      // builder reads each line's flowp/flown/seg col maps and stamps
      // them into one row per fundamental cycle per block.  Skips
      // automatically when there are no cycles (radial network).
      if (is_cycle_basis) {
        const auto& buses = std::get<Collection<BusLP>>(collections);
        const auto& lines = std::get<Collection<LineLP>>(collections);
        const auto& line_commitments =
            std::get<Collection<LineCommitmentLP>>(collections);

        // OTS hookup (issue #509 v1): build a `line_idx → u_col` lookup
        // so the cycle row assembler can switch to the big-M
        // disjunctive form for any cycle that contains a switchable
        // line.  Empty when no LineCommitment rows exist, in which case
        // the lookup is left default-constructed (`add_kvl_rows` then
        // emits the standard equality rows — pre-OTS behaviour).
        kirchhoff::cycle_basis::SwitchableLineLookup switchable_lookup;
        if (!line_commitments.elements().empty()) {
          // Pre-resolve: (LineLP element_index → const LineCommitmentLP*)
          // so the cycle inner loop doesn't re-scan the commitment
          // array per edge.  Use a flat vector indexed by line_index
          // (uses sentinel `nullptr` for non-switchable lines); the
          // line count is small enough that a hash map is overkill.
          const auto n_lines = lines.elements().size();
          std::vector<const LineCommitmentLP*> by_line_idx(n_lines, nullptr);
          for (const auto& lcom : line_commitments.elements()) {
            const auto li =
                static_cast<std::size_t>(lines.element_index(lcom.line_sid()));
            if (li < n_lines) {
              by_line_idx[li] = &lcom;
            }
          }
          switchable_lookup = [by_line_idx = std::move(by_line_idx),
                               &scenario,
                               &stage](std::size_t line_idx, BlockUid buid)
              -> std::optional<kirchhoff::cycle_basis::SwitchableEdge>
          {
            if (line_idx >= by_line_idx.size()) {
              return std::nullopt;
            }
            const auto* lcom = by_line_idx[line_idx];
            if (lcom == nullptr) {
              return std::nullopt;
            }
            const auto col = lcom->lookup_status_col(scenario, stage, buid);
            if (!col.has_value()) {
              return std::nullopt;
            }
            return kirchhoff::cycle_basis::SwitchableEdge {*col};
          };
        }

        kirchhoff::cycle_basis::add_kvl_rows(system_context,
                                             scenario,
                                             stage,
                                             lp,
                                             buses,
                                             lines,
                                             switchable_lookup);
      }

      // node_angle: after all elements are added, check for
      // disconnected bus islands created by inactive lines and pin an
      // orphaned theta variable as a runtime reference if needed.
      if (check_islands) {
        fix_stage_islands(collections, scenario, stage, lp);
      }
    }
  }

  // Planning-level passes — coarse-scope constructs built ONCE per
  // (scene, phase) cell, after the per-(scenario, stage) operational build.
  // Inert until FutureCost (piece 2) / AMPL state vars (piece 5) provide the
  // `add_to_phase_lp` / `add_to_global_lp` hooks.  See add_to_planning_lp.
  add_to_planning_lp(collections, system_context, scene, phase, lp);

  // Compute LP fingerprint before flattening (structured metadata is still
  // available in the raw cols/rows vectors).  Skipped unless the user
  // requested fingerprint output (--lp-fingerprint).
  auto fingerprint = flat_opts.compute_fingerprint
      ? compute_lp_fingerprint(lp.get_cols(), lp.get_rows())
      : LpFingerprint {};

  // Inject scene/phase context so the per-cell LP_QUALITY message
  // carries `[s14 p46]` like other SDDP info lines.  Local copy
  // because the caller's flat_opts is const.
  auto cell_flat_opts = flat_opts;
  cell_flat_opts.flatten_scene_uid = scene.uid();
  cell_flat_opts.flatten_phase_uid = phase.uid();

  auto flat_lp = lp.flatten(cell_flat_opts);
  return std::tuple {std::move(flat_lp), std::move(fingerprint), label_maker};
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

  auto [flat_lp, fingerprint, label_maker] = flatten_from_collections(
      collections, system_context, phase, scene, flat_opts, li.infinity());

  // Install the LabelMaker on the LinearInterface so labels on any
  // cols/rows added after load_flat() use the same LpNamesLevel as
  // the flatten() pass.
  li.set_label_maker(label_maker);

  // Install build-time LP validation thresholds.  When the option is
  // unset/disabled at the consumer site the hooks are O(1) no-ops; the
  // default behavior (enabled with the standard threshold envelope)
  // means validation runs by default for every (scene, phase) LP.
  li.set_validation_options(flat_opts.validation);

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
                      select_codec(flat_opts.memory_codec));
    li.defer_initial_load(std::move(flat_lp));
    // Eagerly compress the snapshot at build time under
    // ``compress`` so the build phase steady-state memory is the
    // compressed snapshot, not the uncompressed flat LP.  Without
    // this, every cell carries an uncompressed
    // ``FlatLinearProblem`` (matval / matind / collb / objval /
    // …) until the first iteration's ``release_backend`` fires —
    // which under ``compress`` on a juan-scale case is hundreds
    // of MB per cell × N cells = a multi-GB peak that the build
    // work pool's ``max_process_rss_mb`` cannot bound (the limit
    // throttles dispatch, not in-flight allocation).
    //
    // After this call the per-cell footprint is ~10× smaller (lz4
    // ratio).  The first SDDP-pool visit decompresses + loads
    // into the live backend via the existing
    // ``ensure_backend`` → ``reconstruct_backend`` →
    // ``apply_post_load_replay`` path; subsequent visits hit the
    // same compressed snapshot.  ``snapshot`` mode keeps its
    // legacy uncompressed-snapshot behaviour (no compression
    // until release).
    if (flat_opts.low_memory_mode == LowMemoryMode::compress) {
      li.enable_compression();
    }
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
  std::get<Collection<CapacityProfileLP>>(colls) =
      make_collection<CapacityProfileLP>(ic, sys.capacity_profile_array);
  std::get<Collection<BatteryLP>>(colls) =
      make_collection<BatteryLP>(ic, sys.battery_array);
  std::get<Collection<ThermalNodeLP>>(colls) =
      make_collection<ThermalNodeLP>(ic, sys.thermal_node_array);
  std::get<Collection<ThermalStorageLP>>(colls) =
      make_collection<ThermalStorageLP>(ic, sys.thermal_storage_array);
  std::get<Collection<HydrogenNodeLP>>(colls) =
      make_collection<HydrogenNodeLP>(ic, sys.hydrogen_node_array);
  std::get<Collection<HydrogenStorageLP>>(colls) =
      make_collection<HydrogenStorageLP>(ic, sys.hydrogen_storage_array);
  std::get<Collection<AmmoniaNodeLP>>(colls) =
      make_collection<AmmoniaNodeLP>(ic, sys.ammonia_node_array);
  std::get<Collection<AmmoniaStorageLP>>(colls) =
      make_collection<AmmoniaStorageLP>(ic, sys.ammonia_storage_array);
  std::get<Collection<CarrierConverterLP>>(colls) =
      make_collection<CarrierConverterLP>(ic, sys.carrier_converter_array);
  std::get<Collection<AllowancePoolLP>>(colls) =
      make_collection<AllowancePoolLP>(ic, sys.allowance_pool_array);
  std::get<Collection<ReserveZoneLP>>(colls) =
      make_collection<ReserveZoneLP>(ic, sys.reserve_zone_array);
  std::get<Collection<ReserveProvisionLP>>(colls) =
      make_collection<ReserveProvisionLP>(ic, sys.reserve_provision_array);
  std::get<Collection<FuelLP>>(colls) =
      make_collection<FuelLP>(ic, sys.fuel_array);
  std::get<Collection<EmissionLP>>(colls) =
      make_collection<EmissionLP>(ic, sys.emission_array);
  std::get<Collection<EmissionZoneLP>>(colls) =
      make_collection<EmissionZoneLP>(ic, sys.emission_zone_array);
  std::get<Collection<EmissionSourceLP>>(colls) =
      make_collection<EmissionSourceLP>(ic, sys.emission_source_array);
  std::get<Collection<CommitmentLP>>(colls) =
      make_collection<CommitmentLP>(ic, sys.commitment_array);
  std::get<Collection<SimpleCommitmentLP>>(colls) =
      make_collection<SimpleCommitmentLP>(ic, sys.simple_commitment_array);
  std::get<Collection<LineCommitmentLP>>(colls) =
      make_collection<LineCommitmentLP>(ic, sys.line_commitment_array);
  // ConverterLP runs after CommitmentLP/SimpleCommitmentLP so the
  // battery's synthesized u_commit columns (created by
  // `expand_batteries`) are already stamped on the LP and can be
  // reused for charge-side gating — "one true source for u_commit".
  std::get<Collection<ConverterLP>>(colls) =
      make_collection<ConverterLP>(ic, sys.converter_array);
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

  // DecisionVariableLP sits just before UserConstraintLP so its free
  // columns are registered with the AMPL resolver before any user
  // constraint that references ``decision_variable("X").value`` is
  // assembled.
  std::get<Collection<DecisionVariableLP>>(colls) =
      make_collection<DecisionVariableLP>(ic, sys.decision_variable_array);

  // PlantLP runs after CommitmentLP / GeneratorLP so the per-(scen,
  // stg) status_cols + generation_cols it references already exist.
  // Placed just before UserConstraintLP so the cap / commit / uniq
  // rows are recorded after every contributor column is built but
  // before any UserConstraint touches the LP.
  std::get<Collection<PlantLP>>(colls) =
      make_collection<PlantLP>(ic, sys.plant_array);

  // UserConstraintLP is the last OPERATIONAL collection so user-constraint rows
  // are added after all other elements whose columns they reference.
  std::get<Collection<UserConstraintLP>>(colls) =
      make_collection<UserConstraintLP>(ic, sys.user_constraint_array);

  // FutureCostLP (planning-only) is populated last so its α cut can reference
  // the reservoir + AMPL terminal state columns built above.  It contributes
  // nothing to the operational stage pass (no add_to_lp); the planning pass
  // (`add_to_planning_lp`) dispatches its `add_to_global_lp` after the stage
  // loop.
  std::get<Collection<FutureCostLP>>(colls) =
      make_collection<FutureCostLP>(ic, sys.future_cost_array);

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
  constexpr auto class_name = LP::Element::class_name.snake_case();
  for (const auto& obj : arr) {
    sim.register_ampl_element(class_name, obj.name, obj.uid);
  }
}

void register_all_ampl_element_names(SimulationLP& sim, const System& sys)
{
  register_element_names<BatteryLP>(sim, sys.battery_array);
  register_element_names<ThermalNodeLP>(sim, sys.thermal_node_array);
  register_element_names<ThermalStorageLP>(sim, sys.thermal_storage_array);
  register_element_names<HydrogenNodeLP>(sim, sys.hydrogen_node_array);
  register_element_names<HydrogenStorageLP>(sim, sys.hydrogen_storage_array);
  register_element_names<AmmoniaNodeLP>(sim, sys.ammonia_node_array);
  register_element_names<AmmoniaStorageLP>(sim, sys.ammonia_storage_array);
  register_element_names<CarrierConverterLP>(sim, sys.carrier_converter_array);
  register_element_names<AllowancePoolLP>(sim, sys.allowance_pool_array);
  register_element_names<BusLP>(sim, sys.bus_array);
  register_element_names<ConverterLP>(sim, sys.converter_array);
  register_element_names<DemandLP>(sim, sys.demand_array);
  register_element_names<EmissionLP>(sim, sys.emission_array);
  register_element_names<EmissionZoneLP>(sim, sys.emission_zone_array);
  register_element_names<EmissionSourceLP>(sim, sys.emission_source_array);
  register_element_names<FlowLP>(sim, sys.flow_array);
  register_element_names<DecisionVariableLP>(sim, sys.decision_variable_array);
  register_element_names<FlowRightLP>(sim, sys.flow_right_array);
  register_element_names<FuelLP>(sim, sys.fuel_array);
  register_element_names<GeneratorLP>(sim, sys.generator_array);
  register_element_names<JunctionLP>(sim, sys.junction_array);
  register_element_names<LineLP>(sim, sys.line_array);
  register_element_names<ReserveProvisionLP>(sim, sys.reserve_provision_array);
  register_element_names<ReserveZoneLP>(sim, sys.reserve_zone_array);
  register_element_names<CommitmentLP>(sim, sys.commitment_array);
  register_element_names<SimpleCommitmentLP>(sim, sys.simple_commitment_array);
  register_element_names<LineCommitmentLP>(sim, sys.line_commitment_array);
  register_element_names<InertiaZoneLP>(sim, sys.inertia_zone_array);
  register_element_names<InertiaProvisionLP>(sim, sys.inertia_provision_array);
  register_element_names<ReservoirLP>(sim, sys.reservoir_array);
  register_element_names<TurbineLP>(sim, sys.turbine_array);
  register_element_names<PumpLP>(sim, sys.pump_array);
  register_element_names<VolumeRightLP>(sim, sys.volume_right_array);
  register_element_names<WaterwayLP>(sim, sys.waterway_array);
  register_element_names<LngTerminalLP>(sim, sys.lng_terminal_array);
  register_element_names<PlantLP>(sim, sys.plant_array);

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

  // AMPL parameter + iterator dispatch tables — populate via the helpers
  // in `ampl_dispatch_registry.cpp` (shims + registration glue).
  register_ampl_param_dispatchers(sim);
  register_ampl_iterator_dispatchers(sim);

  // Class-level compound: `line.flow = +flowp − flown`.
  // Registered once globally; the resolver expands it per-(uid, block).
  {
    constexpr auto line_class = Line::class_name.snake_case();
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

    // Class-level compound: `line.loss = +lossp + lossn`.  Mirrors
    // the unified `Line/loss_sol.parquet` output stream (which is
    // `LP(lossp) + LP(lossn)` per cell — total dissipated energy
    // regardless of direction).  Under the arbitrage-free PWL modes
    // at most one of the two legs is populated per block; the compound
    // resolves correctly either way because the leg whose attribute
    // isn't registered on this (uid, block) silently contributes 0.
    sim.add_ampl_compound(line_class,
                          LineLP::LossName,
                          {
                              AmplCompoundLeg {
                                  .coefficient = +1.0,
                                  .source_attribute = LineLP::LosspName,
                              },
                              AmplCompoundLeg {
                                  .coefficient = +1.0,
                                  .source_attribute = LineLP::LossnName,
                              },
                          });
  }

  // Class-level compound: `converter.flow = +discharge − charge`.
  // Mirrors `line.flow`: positive when the converter discharges the
  // battery into the grid (alias `discharge` → generator generation
  // column), negative when it charges from the grid (alias `charge` →
  // demand load column).  Both leg attributes are registered per-element
  // by `ConverterLP::add_to_lp` via `add_ampl_variable(..., DischargeName,
  // ...)` and `add_ampl_variable(..., ChargeName, ...)`.
  {
    constexpr auto converter_class = Converter::class_name.snake_case();
    sim.add_ampl_compound(converter_class,
                          ConverterLP::FlowName,
                          {
                              AmplCompoundLeg {
                                  .coefficient = +1.0,
                                  .source_attribute = BatteryLP::DischargeName,
                              },
                              AmplCompoundLeg {
                                  .coefficient = -1.0,
                                  .source_attribute = BatteryLP::ChargeName,
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
    constexpr auto line_cls = Line::class_name.snake_case();
    constexpr auto bus_cls = Bus::class_name.snake_case();

    // The three flags are independent:
    //   * !use_kirchhoff suppresses `bus.theta` (theta columns are not
    //     materialized without the Kirchhoff path).
    //   * use_single_bus suppresses the whole `line` class AND
    //     `bus.theta` (LineLP early-exits, Kirchhoff is moot).
    //   * kirchhoff_mode == cycle_basis suppresses `bus.theta` because
    //     the loop-flow formulation enforces KVL via per-cycle Σε·x·f
    //     sums on existing flow vars, so no θ column is ever created
    //     (`BusLP::needs_kirchhoff` short-circuits).
    // When several apply, `use_single_bus=true` is the most fundamental
    // reason and is registered last to win insert_or_assign.
    if (opts.kirchhoff_mode() == KirchhoffMode::cycle_basis) {
      sim.suppress_ampl_attribute(
          bus_cls, BusLP::ThetaName, "kirchhoff_mode=cycle_basis");
    }
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
    for (const auto& cls :
         {
             Generator::class_name.snake_case(),
             Demand::class_name.snake_case(),
             Line::class_name.snake_case(),
             Converter::class_name.snake_case(),
             Battery::class_name.snake_case(),
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
// Process-wide instrumentation for `SystemLP::write_out`.  Updated from
// every cell's write path (parallel under the cell pool) and read once
// at the end of the run by the LP runner.  See
// `SystemLP::total_write_ms` (header) for the rationale and the wall
// vs. cumulative-wall semantics.  Defined inside `namespace gtopt`
// (not in the file-anonymous namespace at the top of this TU) so the
// `SystemLP::write_out` body — which lives here — can name them
// directly, and so the static-getter implementations at the bottom of
// the file can reach the same storage.
namespace
{
std::atomic<double> g_write_out_total_ms {0.0};  // NOLINT
std::atomic<std::size_t> g_write_out_cells {0};  // NOLINT
}  // namespace

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
  m_fingerprint_was_set_ = true;
}

void SystemLP::clear_disposable_collections()
{
  // No-op under `LowMemoryMode::off` — collections must stay live so
  // `release_backend` (also a no-op under off) leaves the live LP
  // self-sufficient for read-back from any element accessor.
  if (m_flat_opts_.low_memory_mode == LowMemoryMode::off) {
    return;
  }
  // No-op when collections are already empty (e.g. rebuild path that
  // hasn't repopulated yet) — guards against silently signalling
  // "built" when the move-assign is a no-no-op.
  if (!m_collections_built_) {
    return;
  }

  std::apply(
      [](auto&... colls)
      {
        const auto try_clear = [](auto& coll) noexcept
        {
          using ElementT = std::remove_cvref_t<decltype(coll)>::value_type;
          if constexpr (!HasUpdateLP<ElementT>) {
            using CollT = std::remove_cvref_t<decltype(coll)>;
            coll = CollT {};
          }
        };
        (try_clear(colls), ...);
      },
      m_collections_);

  // The drop deliberately keeps `m_collections_built_` = true.  Reasoning:
  //  - The resident collections (HasUpdateLP) ARE still fully populated;
  //    production paths that walk only those types (every update_lp call)
  //    must NOT trigger a rebuild.
  //  - Code paths that need the disposable types alive (write_out) check
  //    `m_disposable_collections_built_` and trigger a full rebuild via
  //    `rebuild_collections_if_needed()` if false.
  m_disposable_collections_built_ = false;
}

void SystemLP::drop_for_write_out_done() noexcept
{
  // No-op under `LowMemoryMode::off` — the caller's invariant under
  // off is "backend stays live, collections stay live", and the
  // SDDP/cascade paths that drive off mode rely on it.  Also no-op
  // on the "off" path because there's no per-cell memory pressure
  // there to recover.
  if (m_flat_opts_.low_memory_mode == LowMemoryMode::off) {
    return;
  }

  // Drop the entire collection tuple — strictly more aggressive than
  // `clear_disposable_collections()` because we also empty the
  // HasUpdateLP types that the disposable variant preserves for
  // `update_lp_for_phase`.  On the write_out-done path that future
  // update will never happen, so the wrappers are pure waste.
  //
  // Use `std::apply` to walk every Collection<T> in the tuple and
  // move-assign an empty Collection of the same type, freeing both
  // the element vector and its capacity.
  try {
    std::apply(
        [](auto&... colls)
        {
          const auto drop = [](auto& coll) noexcept
          {
            using CollT = std::remove_cvref_t<decltype(coll)>;
            coll = CollT {};
          };
          (drop(colls), ...);
        },
        m_collections_);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // `noexcept` contract — Collection move-assign is itself nothrow
    // on every used type, but defend against future changes that
    // could throw (e.g. allocator-aware move).  Best-effort: continue.
  }
  m_collections_built_ = false;
  m_disposable_collections_built_ = false;

  // Drop the cut replay journal.  On recovery hot-starts this is
  // typically the largest residue per cell — see
  // `LinearInterface::clear_replay` doc.
  m_linear_interface_.clear_replay();

  // Drop the cached primal/dual/reduced-cost vectors.  After
  // `write_out()` ran, downstream code reads only the parquet on
  // disk; no consumer needs `get_col_sol()` etc. on this cell.
  m_linear_interface_.drop_cached_primal_only();

  // Snapshot may already have been cleared by the caller's
  // `clear_snapshot()`, but be idempotent — `clear()` on an empty
  // holder is a no-op assignment.
  m_linear_interface_.clear_snapshot();
}

void SystemLP::clear_collections_for_eviction() noexcept
{
  // Memory-limited eviction: drop the ENTIRE collection tuple (both the
  // disposable types AND the HasUpdateLP types) while leaving the
  // LinearInterface's compressed snapshot + replay journal intact, so the
  // cell can still reconstruct its backend and replay cuts.  This is the
  // lever that bounds the compress-mode floor — the kept HasUpdateLP
  // wrappers are ~the entire ~25 MB/cell resident state, and keeping them
  // for all `num_cells` is what pinned RSS at ~num_cells × per-cell.
  //
  // Differs from `clear_disposable_collections()` (drops only disposables,
  // keeps HasUpdateLP for `update_lp`) and from `drop_for_write_out_done()`
  // (also tears down snapshot/replay/cache — for cells that will NEVER be
  // touched again).  Here the cell WILL be touched again: `update_lp()` and
  // the other collection readers call `rebuild_collections_if_needed()`,
  // which re-creates the full tuple from the shared `System` on demand.
  //
  // No-op under `off` (collections must stay live) and when already empty.
  if (m_flat_opts_.low_memory_mode == LowMemoryMode::off) {
    return;
  }
  if (!m_collections_built_) {
    return;
  }
  try {
    std::apply(
        [](auto&... colls)
        {
          const auto drop = [](auto& coll) noexcept
          {
            using CollT = std::remove_cvref_t<decltype(coll)>;
            coll = CollT {};
          };
          (drop(colls), ...);
        },
        m_collections_);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Collection move-assign is nothrow on every used type; defend
    // against future allocator-aware changes.  Best-effort.
  }
  m_collections_built_ = false;
  m_disposable_collections_built_ = false;
}

void SystemLP::rebuild_collections_if_needed()
{
  // Skips for `off`: collections are never dropped under off mode.
  // Fires under compress mode: `release_backend()` dropped the
  // disposable XLP types and we need their per-element col indices
  // repopulated before `visit_elements(collections())` can emit
  // meaningful output.  The flatten here is throw-away — it leaves
  // the solver backend untouched so the Phase-2a cached primal/dual
  // still serves `OutputContext`'s value reads, without losing
  // `is_optimal()` to a freshly-loaded-but-unsolved backend.
  //
  // Two-flag gate: triggers if either the full collection set is
  // unbuilt OR the disposable types are dropped (compress mode after
  // `clear_disposable_collections`).  The flatten replays add_to_lp
  // on the freshly-recreated disposable elements; the HasUpdateLP
  // types' state is overwritten too but they receive identical
  // content from the same System data, so `update_lp`'s post-flatten
  // iteration sees the same indices.
  if (m_flat_opts_.low_memory_mode == LowMemoryMode::off) {
    return;
  }
  if (m_collections_built_ && m_disposable_collections_built_) {
    return;
  }

  create_collections(m_system_context_, system(), m_collections_);
  m_collections_built_ = true;
  m_disposable_collections_built_ = true;
  m_system_context_.rebind_system(*this);

  auto flat_opts = m_flat_opts_;
  if (flat_opts.scale_objective == 1.0) {
    flat_opts.scale_objective = system_context().options().scale_objective();
  }
  // The fingerprint was captured during the initial flatten inside
  // `create_lp`; recomputing it would be wasted work.  Also skip
  // LP-name maps — no caller of this rebuild needs them and producing
  // them would waste allocations.
  flat_opts.compute_fingerprint = false;

  // ── Throwaway-rebuild override (2026-05-16) ─────────────────────────
  //
  // The flatten produced here is DISCARDED (`(void)
  // flatten_from_collections(...)` below) — we only consume the
  // `add_to_lp` side effects that repopulate the XLP wrappers' col /
  // row indices inside `m_collections_`.  Every other heavyweight
  // step in `flatten()` is wasted work on this path:
  //
  //   * Ruiz / row-max equilibration computes scaling factors that
  //     the discarded CSC arrays were going to carry.  We don't need
  //     them — kill the entire equilibration pass.
  //   * `compute_stats` builds per-row-type min/max/ratio statistics
  //     for the `LP_QUALITY` log.  Already emitted at solve time —
  //     re-emitting on the write-out rebuild path doubles the cost
  //     and clutters the log.
  //   * Label-string assembly (`col_with_names` / `row_with_names`
  //     and the two `*_name_map` variants) materialises every column
  //     and row label.  No consumer of this rebuild reads them; the
  //     parquet output uses `Id`-based identifiers, not LP labels.
  //   * `scale_objective` divides every objective coefficient by
  //     the global scale factor — pure CPU since the result is
  //     discarded.  Force to 1.0.
  //   * `validation` checks every coefficient / bound / RHS against
  //     LP-quality thresholds and emits spdlog::warn lines.  Already
  //     emitted at solve time; re-firing them on rebuild spams the
  //     log and burns CPU.
  //
  // Profile on juan/IPLP cascade: this slice was 30-50% of the
  // per-cell write-out wall time under `low_memory=compress`.  The
  // overrides below collapse it to little more than a tight
  // `add_to_lp` walk over the elements.
  flat_opts.equilibration_method = LpEquilibrationMethod::none;
  flat_opts.compute_stats = OptBool {false};
  flat_opts.col_with_names = false;
  flat_opts.row_with_names = false;
  flat_opts.col_with_name_map = false;
  flat_opts.row_with_name_map = false;
  flat_opts.scale_objective = 1.0;
  flat_opts.validation.enable = OptBool {false};
  // The CSC matrix produced by `lp.flatten()` is discarded immediately
  // below — only the `add_to_lp` side effects on the XLP wrappers
  // matter.  Bypass the entire flatten body (two CSC passes, row /
  // col bound scans, label materialisation, equilibration) for a
  // 5–10× speedup on the per-cell rebuild slice.
  flat_opts.skip_matrix_build = true;

  // Silence SystemContext registrations (state variables, AMPL
  // variable registry, deferred cross-phase links) on this pass.
  // They were populated during the original flatten and every col/row
  // index is deterministic, so re-running them would be wasted work
  // (and for `defer_state_link` would silently duplicate cross-phase
  // links).
  struct SilentFlattenPassGuard
  {
    SystemContext& ctx;
    SilentFlattenPassGuard(const SilentFlattenPassGuard&) = delete;
    SilentFlattenPassGuard& operator=(const SilentFlattenPassGuard&) = delete;
    SilentFlattenPassGuard(SilentFlattenPassGuard&&) = delete;
    SilentFlattenPassGuard& operator=(SilentFlattenPassGuard&&) = delete;
    explicit SilentFlattenPassGuard(SystemContext& c)
        : ctx(c)
    {
      ctx.set_silent_flatten_pass(/*v=*/true);
    }
    ~SilentFlattenPassGuard() { ctx.set_silent_flatten_pass(/*v=*/false); }
  } const guard {system_context()};

  // Discard the produced FlatLinearProblem; we only care about the
  // `add_to_lp` side effects on the XLP wrappers inside
  // `m_collections_`.  The solver backend is untouched.
  (void)flatten_from_collections(collections(),
                                 system_context(),
                                 phase(),
                                 scene(),
                                 flat_opts,
                                 m_linear_interface_.infinity());
}

void SystemLP::ensure_lp_built()
{
  // Pure backend reconstruct — do NOT run a shadow flatten here.
  // A full flatten allocates hundreds of MB per cell (copy of the
  // entire LP matrix in the local LinearProblem builder); calling it
  // from `ensure_lp_built()` meant every forward/backward phase solve
  // under compress mode did one — tens of thousands of flattens per
  // run.  jemalloc retains that heap for reuse, keeping RSS elevated
  // (~40% of system RAM on the juan case) even though logically the
  // memory is freed.
  //
  // Collections are only needed by callers that read
  // `sys.collections()` — `SystemLP::write_out` and the backward-pass
  // aperture bound-update loop in `sddp_aperture.cpp`.  Those call
  // sites invoke `rebuild_collections_if_needed()` themselves.
  m_linear_interface_.ensure_backend();
}

SystemLP::SystemLP(const System& system,
                   SimulationLP& simulation,
                   PhaseLP phase,
                   SceneLP scene,
                   LpMatrixOptions flat_opts,
                   SystemKind kind)
    : m_system_(system)
    , m_system_context_(simulation, *this, kind)
    , m_phase_(std::move(phase))
    , m_scene_(std::move(scene))
    , m_flat_opts_(std::move(flat_opts))
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

  if (options().use_single_bus()) {
    const auto& buses = system.bus_array;
    if (!buses.empty()) {
      m_single_bus_id_.emplace(buses.front().uid);
    }
  }

  // Populate collections eagerly.  m_collections_ must be assigned
  // in-place so each sub-collection is visible to
  // `InputContext::element_index` as soon as it is built, allowing
  // later collections (e.g. ReserveProvisionLP) to look up earlier
  // ones (e.g. GeneratorLP) without accessing uninitialized memory.
  create_collections(m_system_context_, system, m_collections_);
  m_collections_built_ = true;
  m_disposable_collections_built_ = true;
  create_lp(m_flat_opts_);
  if (m_flat_opts_.low_memory_mode == LowMemoryMode::compress) {
    // Per-element drop: keep HasUpdateLP collections alive (their
    // per-(scen, stg) state — `m_bound_states_`, `m_states_`,
    // `m_coeff_indices_` — must persist across SDDP iterations);
    // free the 22 disposable element types whose only role was
    // to be visited during `add_to_lp` / `flatten`.  This replaces
    // the previous full `m_collections_ = collections_t{}` drop
    // that forced every cell to pay a full-rebuild cost on its
    // first `update_lp` call.
    //
    // `update_lp` no longer triggers `rebuild_collections_if_needed`
    // because its iteration only touches HasUpdateLP types, which
    // are alive.  `write_out` triggers the full rebuild via the
    // two-flag gate when it needs the disposable types.
    clear_disposable_collections();
  }
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
    , m_flat_opts_(std::move(other.m_flat_opts_))
    , m_fingerprint_was_set_(other.m_fingerprint_was_set_)
    , m_collections_built_(other.m_collections_built_)
    , m_disposable_collections_built_(other.m_disposable_collections_built_)
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
    try {
      spdlog::critical(
          "SystemLP move-assign across different System instances — "
          "this would leave m_system_context_ dangling.  Terminating.");
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
    flush_default_logger_best_effort();
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
  m_flat_opts_ = std::move(other.m_flat_opts_);
  m_fingerprint_was_set_ = other.m_fingerprint_was_set_;
  m_collections_built_ = other.m_collections_built_;
  m_disposable_collections_built_ = other.m_disposable_collections_built_;
  m_pending_state_links_ = std::move(other.m_pending_state_links_);
  m_prev_phase_sys_ = other.m_prev_phase_sys_;
  m_system_context_.rebind_system(*this);
  return *this;
}

void SystemLP::write_out()
{
  // Idempotence guard: the SDDP simulation pass writes every cell right
  // after its final solve (backend still live, col_sol / row_dual carry
  // the true solved values).  `PlanningLP::write_out` later iterates
  // all cells too — without this guard the later pass would overwrite
  // the sim-pass output with values read from a freshly rehydrated
  // (and possibly un-solved) backend under compress, breaking
  // solution invariance across low_memory modes.
  if (m_output_written_) {
    return;
  }

  // Optimality guard: there is nothing meaningful to emit for a cell
  // whose LP was never solved to optimum.  Under `low_memory=off` the
  // backend is still live and `col_sol` points to an uninitialised
  // vector of zeros; under `compress` the backend was reconstructed
  // from the build-time snapshot and has no primal values either.
  // Writing in either case produced different numbers of zero-filled
  // parquets across modes (the build-time col_scales and per-field
  // holder filters diverge subtly), breaking solution invariance.
  // Short-circuit here so both modes produce the same "no output"
  // result for unsolved cells.  Cells that were actually solved reach
  // this point in the normal path (SDDP sim pass emits while the
  // backend is live and optimal; monolithic leaves the backend live
  // and optimal).
  if (!linear_interface().is_optimal()) {
    return;
  }

  // Early-exit when no output fields are requested.  Every per-element
  // `add_to_output` short-circuits inside `OutputContext` when its
  // `emit_*` flag is unset, so the only outputs that would land on disk
  // are the (already-written) merged planning JSON and — if explicitly
  // requested — the LP fingerprint.  Skipping the entire rebuild +
  // OutputContext setup + visit pipeline saves the whole write-out slice
  // on cells where the user passed `--write-out none` or no output flag
  // implies emission (compress + diagnostics-only runs).  Mark
  // `m_output_written_` so the idempotence guard fires on subsequent
  // calls from PlanningLP's pool.
  if (options().write_out().atoms == OutputFlags::none) {
    if (options().lp_fingerprint()) {
      const auto fname = as_label(
          "lp_fingerprint", "scene", scene().uid(), "phase", phase().uid());
      const auto filepath = (std::filesystem::path(options().output_directory())
                             / (fname + ".json"))
                                .string();
      write_lp_fingerprint(
          fingerprint(), filepath, scene().uid(), phase().uid());
    }
    m_output_written_ = true;
    return;
  }

  // Collections may have been dropped under LowMemoryMode::compress
  // (end of ctor / release_backend).  Under compress the XLP
  // per-element state (generation_cols, capacity_rows, …) is
  // populated by `add_to_lp` during flatten and cannot be regenerated
  // by a plain `create_collections` — `rebuild_collections_if_needed()`
  // runs a throw-away flatten for those side effects.  Called from
  // here (not from `ensure_lp_built`) so the expensive flatten only
  // runs at output time, not on every forward/backward phase solve.
  // Under off, a plain `create_collections` is sufficient.
  // Per-stage timers for `--trace-log`.  Uses the RUNTIME
  // `spdlog::trace(...)` call (not the compile-level macro) because
  // the CMake PCH pre-includes `<spdlog/spdlog.h>` with
  // `SPDLOG_ACTIVE_LEVEL=INFO` baked in — any `#define
  // SPDLOG_ACTIVE_LEVEL=TRACE` in this TU would come too late.  The
  // runtime check is a cheap atomic load when trace is off.
  using clock = std::chrono::steady_clock;
  const auto t_start = clock::now();

  if (m_flat_opts_.low_memory_mode != LowMemoryMode::off) {
    // Compress drops collections at release_backend() time, so on the
    // write pass we need to re-populate the XLP per-element col/row
    // indices via a throw-away flatten.  Under `off`, collections
    // were never dropped and this is a no-op.
    rebuild_collections_if_needed();
  } else if (!m_collections_built_) {
    create_collections(m_system_context_, system(), m_collections_);
    m_collections_built_ = true;
    m_system_context_.rebind_system(*this);
  }
  const auto t_rebuild = clock::now();

  OutputContext oc(system_context(),
                   linear_interface(),
                   scene().uid(),
                   phase().uid(),
                   phase().is_continuous());
  const auto t_oc = clock::now();

  auto count = visit_elements(collections(),
                              [&oc](const auto& e)
                              {
                                using T = std::decay_t<decltype(e)>;
                                // Skip at compile time any element without an
                                // `add_to_output`; the `HasAddToOutput`
                                // capability is the discriminator (mirrors the
                                // `HasAddToLp` gating in `add_to_lp`).  This
                                // also picks up output-only / planning-level
                                // elements that have no per-block `add_to_lp`.
                                if constexpr (!HasAddToOutput<T>) {
                                  return true;
                                } else {
                                  return e.add_to_output(oc);
                                }
                              });
  const auto t_visit = clock::now();

  if (count <= 0) {
    SPDLOG_WARN("No elements added to output");
    return;
  }

  oc.write();
  const auto t_write = clock::now();

  const auto total_cell_ms =
      std::chrono::duration<double, std::milli>(t_write - t_start).count();

  spdlog::trace(
      "SystemLP::write_out [scene={} phase={}]: "
      "rebuild_coll={:.1f}ms, OutputContext={:.1f}ms, "
      "visit_elements={:.1f}ms, oc.write={:.1f}ms, total={:.1f}ms",
      scene().uid(),
      phase().uid(),
      std::chrono::duration<double, std::milli>(t_rebuild - t_start).count(),
      std::chrono::duration<double, std::milli>(t_oc - t_rebuild).count(),
      std::chrono::duration<double, std::milli>(t_visit - t_oc).count(),
      std::chrono::duration<double, std::milli>(t_write - t_visit).count(),
      total_cell_ms);

  // Feed the process-wide counters used by the runner's final
  // "Write output time" report.  See `SystemLP::total_write_ms` doc
  // for why the runner's own stopwatch is misleading under SDDP.
  g_write_out_total_ms.fetch_add(total_cell_ms, std::memory_order_relaxed);
  g_write_out_cells.fetch_add(1, std::memory_order_relaxed);

  // Write LP fingerprint if requested
  if (options().lp_fingerprint()) {
    const auto fname = as_label(
        "lp_fingerprint", "scene", scene().uid(), "phase", phase().uid());
    const auto filepath = (std::filesystem::path(options().output_directory())
                           / (fname + ".json"))
                              .string();
    write_lp_fingerprint(fingerprint(), filepath, scene().uid(), phase().uid());
  }

  m_output_written_ = true;
}

void SystemLP::accumulate_convergence_indicators(SceneIndex scene_index,
                                                 PhaseIndex phase_index)
{
  // Precondition: the LP has just been solved to optimum (the SDDP
  // forward pass is the only caller and gates on `result.has_value()
  // && solve_status == 0`).  Reading slack column values out of an
  // unsolved backend would silently return zero, so short-circuit here
  // if a future caller forgets the gate.
  auto& li = linear_interface();
  if (!li.is_optimal()) {
    return;
  }

  auto& stats = li.mutable_solver_stats();
  const auto col_sol = li.get_col_sol();

  // Tiny adapter: read an optional<ColIndex> as a weighted col-sol
  // value, folding "column not created" into a no-op `0.0`.  The slack
  // columns are inherently sparse (created only when the corresponding
  // *_cost is set on the input element), so the optional gate fires for
  // most (scenario, stage, block, element) cells on a typical model.
  const auto weighted = [&col_sol](std::optional<ColIndex> c,
                                   double w) noexcept -> double
  {
    return c.transform([&](auto col) { return w * col_sol[col]; })
        .value_or(0.0);
  };

  // m³/s × 1 h ÷ 1e6 m³/hm³ → block-duration must already be in hours,
  // which `BlockLP::duration()` guarantees throughout gtopt.
  constexpr double m3s_h_to_hm3 = 3600.0 / 1.0e6;

  for (auto&& [scenario, stage] :
       std::views::cartesian_product(scene().scenarios(), phase().stages()))
  {
    const double pw = scenario.probability_factor();

    // ── Demand.fail  →  unserved_demand [MWh] ──────────────────────────
    //
    // Post-P0 the `fail` LP variable is gone; `DemandLP::fail_sol_at`
    // reconstructs the value from cached `block_lmax_values_` and
    // `load_cols`'s primal solution (`max(0, lmax − load_sol)`).
    // Same per-block scaling (probability × duration) as the
    // pre-substitution `weighted(fail_col, ...)` path.
    for (auto&& [d, blk] :
         std::views::cartesian_product(elements<DemandLP>(), stage.blocks()))
    {
      stats.unserved_demand +=
          d.fail_sol_at(scenario, stage, blk, col_sol) * pw * blk.duration();
    }

    // ── FlowRight.fail  →  unserved_flow [hm³] ─────────────────────────
    //
    // Post-P0 the `fail` LP variable is gone; `FlowRightLP::fail_sol_at`
    // reconstructs the value from cached `block_discharge_values_` and
    // `flow_cols`'s primal solution (`max(0, discharge − flow_sol)`).
    // Same per-block scaling (probability × duration × m³/s·h → hm³) as
    // the pre-substitution `weighted(fail_col, ...)` path.
    for (auto&& [f, blk] :
         std::views::cartesian_product(elements<FlowRightLP>(), stage.blocks()))
    {
      stats.unserved_flow += f.fail_sol_at(scenario, stage, blk, col_sol) * pw
          * blk.duration() * m3s_h_to_hm3;
    }

    // ── Reservoir.soft_emin / efin_slack  →  hm³ ───────────────────────
    //
    // Only ReservoirLP is included here: BatteryLP and LngTerminalLP
    // share the same `soft_emin_col_at` / `efin_slack_col_at` accessors
    // (inherited from `StorageLP<>`) but their slack columns are in MWh
    // and m³ respectively, so summing them into hm³ accumulators would
    // mix units.  Extend with separate fields if those storage families
    // ever need their own convergence indicators.
    for (const auto& r : elements<ReservoirLP>()) {
      stats.soft_emin_deficit +=
          weighted(r.soft_emin_col_at(scenario, stage), pw);
      stats.efin_deficit += weighted(r.efin_slack_col_at(scenario, stage), pw);
    }
  }

  // scene_index / phase_index are accepted for callsite consistency with
  // the surrounding SDDP forward-pass logs; the SystemLP already owns
  // the corresponding `scene()` / `phase()` LP objects so we read the
  // uids straight from them rather than going back through the
  // SimulationLP.  Cast suppresses unused-parameter warnings under
  // builds that disable spdlog::trace at compile time.
  (void)scene_index;
  (void)phase_index;
  spdlog::trace(
      "SystemLP::accumulate_convergence_indicators [scene={} phase={}]: "
      "UD={:.3g} UF={:.3g} SEm={:.3g} Efin={:.3g}",
      scene().uid(),
      phase().uid(),
      stats.unserved_demand,
      stats.unserved_flow,
      stats.soft_emin_deficit,
      stats.efin_deficit);
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
  auto& li = linear_interface();
  auto result = li.resolve(solver_options);
  if (!result) {
    return result;
  }

  // ── Fix-integers dual-recovery pass ───────────────────────────────────
  //
  // A MIP solution carries no LP duals (shadow prices) or reduced costs,
  // so a UC run with binary commitment columns would write empty
  // `balance_dual.parquet` and no bus LMPs.  When the caller requested
  // duals and/or reduced costs AND the problem actually contains integer
  // columns, run the standard recovery technique: pin every integer to
  // its rounded MIP value, relax integrality, and re-solve the resulting
  // pure LP — whose row duals / reduced costs are the correct marginal
  // prices given the committed integer solution.  The primal stays the
  // MIP primal (integers pinned to their optimal values).
  //
  // Gated tightly so pure-LP solves and runs that don't need duals pay
  // nothing: the `has_integer_cols()` scan only runs when dual/rc output
  // is requested, and `fix_integers_and_resolve` itself is a no-op when
  // no integer column exists.  Enable crossover on the follow-up solve so
  // a barrier backend produces clean vertex duals.
  const auto sel = options().write_out();
  const bool wants_duals = has_flag(sel.atoms, OutputFlags::dual)
      || has_flag(sel.atoms, OutputFlags::reduced_cost);
  if (wants_duals && li.has_integer_cols()) {
    auto lp_opts = solver_options;
    lp_opts.crossover = true;
    auto fix = li.fix_integers_and_resolve(lp_opts);
    if (!fix) {
      // The MIP solve already succeeded; a failed dual-recovery re-solve
      // should not fail the whole cell.  Warn and keep the MIP result
      // (duals stay empty, primal is intact).
      spdlog::warn(
          "SystemLP::resolve [scene={} phase={}]: fix-integers dual "
          "recovery re-solve failed: {} (keeping MIP primal, duals "
          "unavailable)",
          scene().uid(),
          phase().uid(),
          fix.error().message);
      return result;  // original MIP status
    }
    if (fix->fixed_columns > 0) {
      SPDLOG_DEBUG(
          "SystemLP::resolve [scene={} phase={}]: fixed {} integer "
          "column(s) and re-solved LP for duals (status {})",
          scene().uid(),
          phase().uid(),
          fix->fixed_columns,
          fix->status.value_or(0));
    }
  }

  return result;
}

int SystemLP::update_lp()
{
  // Trigger an `ensure_backend()` before querying
  // `supports_set_coeff`, which dereferences `m_backend_` directly.
  m_linear_interface_.ensure_backend();
  if (!linear_interface().supports_set_coeff()) {
    return 0;
  }

  // Rebuild collections if they were dropped.  Self-gating
  // (`rebuild_collections_if_needed` returns immediately when both
  // built-flags are set), so this is a NO-OP under the default
  // keep-resident behaviour (P3) and under disposable-only drop —
  // `visit_elements` below walks the still-resident HasUpdateLP types.
  //
  // It only does work under the memory-limited *full* eviction path
  // (`clear_collections_for_eviction()` on release), where the
  // HasUpdateLP types this loop iterates were also dropped: without the
  // rebuild, `visit_elements` would silently walk EMPTY collections and
  // skip the per-(scene,stage) coefficient updates (seepage, production
  // factors, …) — a silent-wrong-result bug, not a crash.  The rebuild
  // is the optimized flatten (`skip_matrix_build`, no equilibration /
  // labels / stats), ~5-10× cheaper than a full flatten — the CPU side
  // of the bounded-memory trade, paid only when a memory limit forces
  // eviction.
  rebuild_collections_if_needed();

  int total = 0;

  for (auto&& scenario : scene().scenarios()) {
    for (auto&& stage : phase().stages()) {
      visit_elements(collections(),
                     [&total, this, &scenario, &stage](auto& element) -> bool
                     {
                       using T = std::decay_t<decltype(element)>;
                       if constexpr (HasUpdateLP<T>) {
                         try {
                           total += element.update_lp(*this, scenario, stage);
                         } catch (const std::exception& ex) {
                           SPDLOG_ERROR(
                               std::format("Error updating {} uid={} in LP "
                                           "(scenario={}, stage={}): {}",
                                           T::Element::class_name,
                                           element.uid(),
                                           scenario.uid(),
                                           stage.uid(),
                                           ex.what()));
                           throw;
                         }
                       }
                       return true;
                     });
    }
  }

  return total;
}

// Static getters for the process-wide write_out counters defined in
// the anonymous namespace.  Implemented here (rather than inline in
// the header) so the atomics' definition stays in the .cpp TU.
double SystemLP::total_write_ms() noexcept
{
  return g_write_out_total_ms.load(std::memory_order_relaxed);
}

std::size_t SystemLP::total_write_cells() noexcept
{
  return g_write_out_cells.load(std::memory_order_relaxed);
}

}  // namespace gtopt
