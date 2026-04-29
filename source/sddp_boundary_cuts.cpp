/**
 * @file      sddp_boundary_cuts.cpp
 * @brief     Boundary (future-cost) cuts for SDDP — load path.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from `source/sddp_cut_io.cpp`.
 */

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

#include <gtopt/as_label.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_io_internal.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/utils.hpp>

#ifndef SPDLOG_ACTIVE_LEVEL
#  define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include <spdlog/spdlog.h>

namespace gtopt
{
using namespace gtopt::detail;

[[nodiscard]] auto load_boundary_cuts_csv(
    PlanningLP& planning_lp,
    const std::string& filepath,
    const SDDPOptions& options,
    [[maybe_unused]] const LabelMaker& label_maker,
    [[maybe_unused]] const StrongIndexVector<
        SceneIndex,
        StrongIndexVector<PhaseIndex, PhaseStateInfo>>& scene_phase_states)
    -> std::expected<CutLoadResult, Error>
{
  // ── Mode check ────────────────────────────────────────────────
  const auto mode = options.boundary_cuts_mode;
  if (mode == BoundaryCutsMode::noload) {
    SPDLOG_INFO("SDDP: boundary cuts mode is 'noload' -- skipping");
    return CutLoadResult {};
  }

  const bool separated = (mode == BoundaryCutsMode::separated);

  try {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format(
              "Cannot open boundary cuts file for reading: {}", filepath),
      });
    }

    // ── Parse header ────────────────────────────────────────────
    // Expected: name,iteration,scene,rhs,StateVar1,StateVar2,...
    // (Legacy format without iteration column is auto-detected.)
    std::string header_line;
    std::getline(ifs, header_line);
    strip_cr(header_line);

    std::vector<std::string> headers;
    {
      std::istringstream hss(header_line);
      std::string token;
      while (std::getline(hss, token, ',')) {
        headers.push_back(token);
      }
    }

    // Detect whether the CSV has the `iteration` column.
    const bool has_iteration_col =
        (headers.size() >= 2 && headers[1] == "iteration");
    const int state_var_start = has_iteration_col ? 4 : 3;

    if (std::cmp_less(headers.size(), state_var_start + 1)) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message =
              std::format("Boundary cuts CSV must have at least {} columns "
                          "(name,[iteration,]scene,rhs,<state_vars>); got {}",
                          state_var_start + 1,
                          headers.size()),
      });
    }

    // ── Determine last phase and build name->column mapping ─────
    const auto& sim = planning_lp.simulation();
    const auto num_scenes = sim.scene_count();
    const auto last_phase = sim.last_phase_index();

    // Build scene UID -> SceneIndex lookup (for "separated" mode)
    std::unordered_map<SceneUid, SceneIndex, std::hash<SceneUid>>
        scene_uid_to_index;
    map_reserve(scene_uid_to_index, static_cast<std::size_t>(num_scenes));
    for (const auto si : iota_range<SceneIndex>(0, num_scenes)) {
      scene_uid_to_index[sim.uid_of(si)] = si;
    }

    // Build element-name -> uid lookup from the System.
    const auto& sys = planning_lp.planning().system;

    std::unordered_map<std::string, std::pair<std::string_view, Uid>>
        name_to_class_uid;
    map_reserve(name_to_class_uid,
                sys.junction_array.size() + sys.battery_array.size()
                    + sys.reservoir_array.size());
    for (const auto& junc : sys.junction_array) {
      name_to_class_uid[junc.name] = {"Junction", junc.uid};
    }
    for (const auto& bat : sys.battery_array) {
      name_to_class_uid[bat.name] = {"Battery", bat.uid};
    }
    // Hydro reservoirs are state variables in every CEN-style SDDP
    // case (the dominant boundary-cut state column).  Without this
    // entry, the lookup at lines below misses on ``Reservoir:<name>``
    // headers, the loader reports "state variable not found", and
    // either the row is skipped (``skip_cut``) or the coefficient is
    // silently zeroed (``skip_coeff``) — both modes drop information
    // that the upstream PLP solver intended to ship.  Captured by
    // ``test_sddp_boundary_cuts_reservoir_name_map.cpp``.
    for (const auto& rsv : sys.reservoir_array) {
      name_to_class_uid[rsv.name] = {"Reservoir", rsv.uid};
    }

    // For each state-variable header column, find the
    // corresponding LP column in the last phase.
    const auto& svar_map = sim.state_variables(first_scene_index(), last_phase);

    const auto num_state_cols = std::ssize(headers) - state_var_start;
    std::vector<std::optional<ColIndex>> header_col_map;
    header_col_map.reserve(num_state_cols);
    std::vector<std::string> header_names;
    header_names.reserve(num_state_cols);

    for (auto hi = static_cast<std::size_t>(state_var_start);
         hi < headers.size();
         ++hi)
    {
      const auto& hdr = headers[hi];
      std::optional<ColIndex> found_col;

      // Parse "ClassName:ElementName" or plain "ElementName"
      std::string_view class_filter;
      std::string element_name;
      if (const auto colon = hdr.find(':'); colon != std::string::npos) {
        class_filter = std::string_view(hdr).substr(0, colon);
        element_name = hdr.substr(colon + 1);
      } else {
        element_name = hdr;
      }

      // Look up element name -> (class_name, uid)
      if (auto it = name_to_class_uid.find(element_name);
          it != name_to_class_uid.end())
      {
        const auto& [cname, elem_uid] = it->second;
        if (!class_filter.empty() && class_filter != cname) {
          SPDLOG_WARN(
              "Boundary cuts: header '{}' class '{}' does "
              "not match element '{}' class '{}'; skipping",
              hdr,
              class_filter,
              element_name,
              cname);
        } else {
          // Match (class, uid, col_name=='efin') — class is part of
          // the key.  Without it, two classes with the same uid
          // (e.g. Junction:j_up and Reservoir:rsv1 both at uid=1 in
          // the 3-phase test fixture) would race for the first match
          // by uid alone, silently substituting a column from the
          // wrong class.  In particular, plain Junctions don't
          // register `efin` state variables (only Reservoir/Battery
          // do), so a header `j_up` would resolve to Reservoir:1's
          // column under the bare-uid match — surprising the caller
          // who asked for a junction.  Class-aware match makes the
          // resolution intent-faithful: when the requested class has
          // no state variable, found_col stays nullopt and the
          // missing-coefficient handling (skip_coeff or skip_cut)
          // takes over.
          for (const auto& [key, svar] : svar_map) {
            if (key.uid == elem_uid && key.class_name == cname
                && is_final_state_col(key.col_name))
            {
              found_col = svar.col();
              break;
            }
          }
        }
      }

      header_col_map.push_back(found_col);
      header_names.push_back(hdr);
    }

    // Track which missing state variables have already been warned about.
    std::set<std::size_t> warned_missing_cols;

    // ── Pre-scan: collect all rows for max_iterations filtering ──
    struct RawBoundaryCut
    {
      std::string name;
      IterationIndex iteration_index {};
      SceneUid scene_uid {};
      double rhs;
      std::string coeff_line;
    };

    std::vector<RawBoundaryCut> raw_cuts;
    std::string line;
    while (std::getline(ifs, line)) {
      strip_cr(line);
      if (line.empty()) {
        continue;
      }

      std::istringstream iss(line);
      std::string token;

      // Column 0: name
      std::getline(iss, token, ',');
      auto cut_name = token;

      IterationIndex iteration_index {};
      if (has_iteration_col) {
        // Column 1: iteration
        std::getline(iss, token, ',');
        iteration_index = IterationIndex {std::stoi(token)};
      }

      // Next column: scene UID
      std::getline(iss, token, ',');
      const SceneUid scene_uid = make_uid<Scene>(std::stoi(token));

      // Next column: rhs
      std::getline(iss, token, ',');
      const auto rhs = std::stod(token);

      // The rest contains the coefficient values
      std::string remainder;
      std::getline(iss, remainder);

      raw_cuts.push_back(RawBoundaryCut {
          .name = std::move(cut_name),
          .iteration_index = iteration_index,
          .scene_uid = scene_uid,
          .rhs = rhs,
          .coeff_line = std::move(remainder),
      });
    }

    // ── Filter by max_iterations ────────────────────────────────
    const auto max_iters = options.boundary_max_iterations;
    if (max_iters > 0 && has_iteration_col) {
      std::set<int> distinct_iters;
      for (const auto& rc : raw_cuts) {
        distinct_iters.insert(rc.iteration_index);
      }
      if (std::cmp_greater(distinct_iters.size(), max_iters)) {
        std::set<int> keep_iters;
        auto it = distinct_iters.end();
        for (int i = 0; i < max_iters; ++i) {
          --it;
          keep_iters.insert(*it);
        }
        std::erase_if(raw_cuts,
                      [&keep_iters](const RawBoundaryCut& rc)
                      { return !keep_iters.contains(rc.iteration_index); });
        SPDLOG_INFO(
            "SDDP: boundary cuts filtered to last {} "
            "iterations ({} cuts)",
            max_iters,
            raw_cuts.size());
      }
    }

    // ── α is already registered on every phase (including the
    //    last) by `SDDPMethod::initialize_alpha_variables`, pinned
    //    at `lowb = uppb = 0`.  Each successful boundary-cut
    //    install below calls `free_alpha(…, last_phase)` to
    //    release that pin — same contract as the backward-pass
    //    cut sites.  No separate add-α pass is needed here.

    // ── Add cuts to the LP ──────────────────────────────────────
    // Pre-fix this scope captured `sa = effective_scale_alpha(...)`
    // and `scale_obj = bdr_li.scale_objective()` to pre-divide and
    // pre-multiply the cut row's RHS / coefficients.  Both factors
    // are now applied by `LinearInterface::compose_physical` itself
    // (step 1: × col_scale, step 1b: ÷ scale_objective), so the
    // assembly below emits pure physical-space rows and the locals
    // are no longer needed.

    // When boundary_cuts_valuation == present_value, apply the
    // effective discount factor of the last phase's last stage to
    // the cut RHS and coefficients.  This brings present-value cuts
    // into the same basis as the LP objective.
    const auto valuation = sim.simulation().boundary_cuts_valuation.value_or(
        BoundaryCutsValuation::end_of_horizon);
    double bc_discount = 1.0;
    if (valuation == BoundaryCutsValuation::present_value) {
      const auto& last_phase_lp = sim.phases()[last_phase];
      if (!last_phase_lp.stages().empty()) {
        bc_discount = last_phase_lp.stages().back().discount_factor();
        SPDLOG_INFO(
            "Boundary cuts: applying present-value discount factor {:.6f}",
            bc_discount);
      }
    }

    IterationIndex max_iteration {};
    const auto missing_mode = options.missing_cut_var_mode;
    int cuts_loaded = 0;
    int cuts_skipped = 0;
    for (const auto& rc : raw_cuts) {
      // Pre-scan: check for missing state variables with non-zero
      // coefficients.
      bool has_missing = false;
      {
        std::istringstream scan_ss(rc.coeff_line);
        std::string tok;
        for (std::size_t ci = 0; ci < header_col_map.size(); ++ci) {
          if (!std::getline(scan_ss, tok, ',')) {
            break;
          }
          if (!header_col_map[ci].has_value() && !tok.empty()
              && std::stod(tok) != 0.0)
          {
            has_missing = true;
            if (!warned_missing_cols.contains(ci)) {
              warned_missing_cols.insert(ci);
              SPDLOG_WARN(
                  "Boundary cuts: state variable '{}' not "
                  "found in the current model; non-zero "
                  "coefficient {} (mode={})",
                  header_names[ci],
                  tok,
                  enum_name(missing_mode));
            }
          }
        }
      }
      if (has_missing && missing_mode == MissingCutVarMode::skip_cut) {
        ++cuts_skipped;
        continue;
      }

      // Determine which scenes get this cut
      SceneIndex scene_start {0};
      SceneIndex scene_end {num_scenes};
      if (separated) {
        auto it = scene_uid_to_index.find(rc.scene_uid);
        if (it == scene_uid_to_index.end()) {
          SPDLOG_TRACE(
              "Boundary cut '{}' scene UID {} not found in "
              "scene_array -- skipping",
              rc.name,
              rc.scene_uid);
          continue;
        }
        scene_start = it->second;
        scene_end = next(it->second);
      }

      for (const auto scene_index : iota_range(scene_start, scene_end)) {
        const auto* alpha_svar = find_alpha_state_var(
            planning_lp.simulation(), scene_index, last_phase);
        if (alpha_svar == nullptr) {
          continue;  // No α on this (scene, phase) — nothing to add
        }

        // IterationContext carries a per-cut (iter, offset) pair
        // so multiple boundary cuts for the same (scene, phase)
        // don't collide on the metadata-based duplicate detector
        // in `LinearInterface::add_row`.  The iteration is pulled
        // from the cut name; `cuts_loaded` breaks ties when the
        // same iteration emits multiple cuts.
        // Build the row in PURE PHYSICAL units.  `add_cut_row`
        // forwards through `LinearInterface::compose_physical`
        // (`linear_interface.cpp:1118-1200`) which applies the
        // `× col_scale` and `÷ scale_objective` exactly once.  The
        // earlier form pre-divided by `scale_obj` and pre-multiplied
        // by `col_scale` here, so `compose_physical` ended up applying
        // both factors a second time → `get_row_low()` returned
        // `phys_rhs / scale_obj` instead of `phys_rhs`, breaking the
        // documented round-trip invariant
        // (`compose_physical` block comment, lines 1142-1146) and
        // making boundary cuts effectively non-binding under
        // `scale_obj != 1`.  Captured by
        // `test_sddp_boundary_cuts_round_trip.cpp` (audit P0-1).
        //
        // `row.scale = 1.0` (the `SparseRow` default) leaves
        // `composite_scale` to be filled by `compose_physical`'s
        // step 1b alone, matching the canonical SDDP optimality cut
        // built by `build_benders_cut_physical` overload 1
        // (`benders_cut.cpp:100-134`) — same row shape, same dual
        // semantics.
        auto row = SparseRow {
            .lowb = rc.rhs * bc_discount,
            .uppb = LinearProblem::DblMax,
            .class_name = sddp_boundary_cut_class_name,
            .constraint_name = sddp_loaded_cut_constraint_name,
            .variable_uid = sim.uid_of(last_phase),
            .context = make_iteration_context(
                sim.uid_of(scene_index),
                sim.uid_of(last_phase),
                uid_of(extract_iteration_from_name(std::string_view {rc.name})),
                cuts_loaded),
        };
        // α physical coefficient = 1.0 (canonical optimality form
        // ``α ≥ Σᵢ rcᵢ · xᵢ + b``).  ``compose_physical`` step 1
        // multiplies by ``col_scales[α] = scale_alpha`` and step 1b
        // divides by ``scale_obj``; the resulting LP-space coefficient
        // matches the `build_benders_cut_physical` reference path.
        row[alpha_svar->col()] = 1.0;

        std::istringstream coeff_ss(rc.coeff_line);
        std::string token;
        for (const auto& col_opt : header_col_map) {
          if (!std::getline(coeff_ss, token, ',')) {
            break;
          }
          if (!col_opt.has_value()) {
            continue;  // skip_coeff: drop missing coefficient
          }
          const auto coeff = std::stod(token);
          if (coeff != 0.0) {
            // State-variable physical coefficient in $/(physical-unit).
            // `compose_physical` applies the column scale itself.
            row[*col_opt] = -coeff * bc_discount;
          }
        }

        // Boundary cuts encode the future-cost function at the
        // horizon (optimality-style: α ≥ state-dependent RHS on the
        // last phase).  Routed through the unified `add_cut_row`
        // helper: since the row always carries α (set above via
        // `row[alpha_svar->col()] = 1.0`), the Optimality gate
        // releases the bootstrap pin.  Pass `cut_coeff_eps` so loaded
        // cuts share the same numerical-noise filter that the SDDP
        // backward-pass uses for generated cuts (audit P2-1) — without
        // it, ``add_cut_row`` defaulted to ``eps=0`` and small
        // physical coefficients (e.g., ill-conditioned PLP exports)
        // could survive into the LP and degrade kappa.
        std::ignore = add_cut_row(planning_lp,
                                  scene_index,
                                  last_phase,
                                  CutType::Optimality,
                                  row,
                                  options.cut_coeff_eps);
      }
      max_iteration = std::max(max_iteration, rc.iteration_index);
      ++cuts_loaded;
    }

    if (cuts_skipped > 0) {
      SPDLOG_WARN(
          "Boundary cuts: skipped {} cut(s) referencing "
          "missing state variables",
          cuts_skipped);
    }

    SPDLOG_INFO(
        "SDDP: loaded {} boundary cuts from {} (mode={}, "
        "max_iters={})",
        cuts_loaded,
        filepath,
        enum_name(mode),
        max_iters);
    return CutLoadResult {
        .count = cuts_loaded,
        .max_iteration = max_iteration,
    };

  } catch (const std::exception& e) {
    return std::unexpected(Error {
        .code = ErrorCode::FileIOError,
        .message = std::format(
            "Error loading boundary cuts from {}: {}", filepath, e.what()),
    });
  }
}

}  // namespace gtopt
