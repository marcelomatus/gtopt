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
#include <set>
#include <unordered_map>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <gtopt/as_label.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/future_cost_lp.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_cut_io_internal.hpp>
#include <gtopt/sddp_types.hpp>  // sddp_boundary_cut_tag
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
  // Terminal-phase multicut: the boundary cut for source scenario
  // `rc.scene_uid` is broadcast onto `varphi_{source}` in EVERY scene's
  // terminal LP — the horizon analogue of the intermediate-phase multicut
  // routing in share_cuts_for_phase.  Requires register_alpha_variables to
  // have laid down N terminal α columns, which it does iff
  // boundary_cut_sharing == multicut.
  const bool multicut =
      (options.boundary_cut_sharing == BoundaryCutSharingMode::multicut);

  try {
    // ── Read the CSV with the Arrow CSV reader ───────────────────
    // Arrow's CSV reader handles CRLF / whitespace / quoting / type
    // coercion uniformly and gives us a typed in-memory Table that
    // downstream code can iterate column-major.  We pin explicit
    // types for the structured columns (iteration / scene as int32;
    // rhs and every state-variable column as float64) so a malformed
    // row surfaces as an Arrow conversion error here instead of
    // silently feeding ``std::stoi`` / ``std::stod`` a bad token
    // further down the pipeline.
    //
    // The state-variable columns are unknown at read-configure time
    // (they vary per case); we leave their types unset in
    // ``column_types`` and rely on Arrow's automatic int/float
    // inference plus a single explicit cast to float64 below.
    auto maybe_infile = arrow::io::ReadableFile::Open(filepath);
    if (!maybe_infile.ok()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot open boundary cuts file for "
                                 "reading: {} ({})",
                                 filepath,
                                 maybe_infile.status().ToString()),
      });
    }

    auto read_options = arrow::csv::ReadOptions::Defaults();
    auto parse_options = arrow::csv::ParseOptions::Defaults();
    auto convert_options = arrow::csv::ConvertOptions::Defaults();
    convert_options.column_types["iteration"] = arrow::int32();
    convert_options.column_types["scene"] = arrow::int32();
    convert_options.column_types["rhs"] = arrow::float64();
    // Permit a missing ``iteration`` column (legacy file without
    // iteration tracking — see ``has_iteration_col`` below).  Arrow
    // would otherwise raise on the unconsumed entry in column_types.
    convert_options.include_missing_columns = false;

    auto maybe_reader =
        arrow::csv::TableReader::Make(arrow::io::default_io_context(),
                                      *maybe_infile,
                                      read_options,
                                      parse_options,
                                      convert_options);
    if (!maybe_reader.ok()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Cannot create CSV reader for boundary cuts "
                                 "{}: {}",
                                 filepath,
                                 maybe_reader.status().ToString()),
      });
    }
    auto maybe_table = (*maybe_reader)->Read();
    if (!maybe_table.ok()) {
      return std::unexpected(Error {
          .code = ErrorCode::FileIOError,
          .message = std::format("Error parsing boundary cuts CSV {}: {}",
                                 filepath,
                                 maybe_table.status().ToString()),
      });
    }
    const auto& table = *maybe_table;
    const auto schema = table->schema();

    // Extract header names from the Arrow schema, preserving file
    // order.  These drive both the layout-detection branch
    // (``has_iteration_col``) and the per-column state-variable
    // lookup below.
    std::vector<std::string> headers;
    headers.reserve(static_cast<std::size_t>(schema->num_fields()));
    for (int i = 0; i < schema->num_fields(); ++i) {
      headers.push_back(schema->field(i)->name());
    }

    // Detect whether the CSV has the `iteration` column.
    const bool has_iteration_col =
        !headers.empty() && headers[0] == "iteration";
    const int state_var_start = has_iteration_col ? 3 : 2;

    if (std::cmp_less(headers.size(), state_var_start + 1)) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message =
              std::format("Boundary cuts CSV must have at least {} columns "
                          "([iteration,]scene,rhs,<state_vars>); got {}",
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
                    + sys.reservoir_array.size()
                    + sys.volume_right_array.size());
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
    // VolumeRights are Tilmant "dummy reservoir" SDDP couplers (default
    // ``use_state_variable = true``) and register the same storage
    // ``efin`` state variable — so ``VolumeRight:<name>`` cut columns
    // (incl. the irrigation economy accumulators) must resolve too.
    // Captured by ``test_sddp_boundary_cuts_volume_right.cpp``.
    for (const auto& vr : sys.volume_right_array) {
      name_to_class_uid[vr.name] = {"VolumeRight", vr.uid};
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

    // ── Pre-scan: pull typed columns from the Arrow Table ────────
    // ``RawBoundaryCut`` no longer carries a ``name`` field (the CSV
    // leading-name column was retired in 2026-05).  Coefficients live
    // in ``coeffs`` indexed by (cut, state_col) — chunked-array
    // friendly so an Arrow Table with multiple record batches still
    // works without forcing a single big copy.
    struct RawBoundaryCut
    {
      IterationIndex iteration_index {};
      SceneUid scene_uid {};
      double rhs {};
      // Coefficients in the same order as ``header_col_map`` /
      // ``header_names`` (i.e. file order of the trailing state-var
      // columns).  Missing / null cells appear as NaN to mark
      // "absent" for the downstream missing-cut-var handling.
      std::vector<double> coeffs;
    };

    auto iteration_col =
        has_iteration_col ? table->GetColumnByName("iteration") : nullptr;
    auto scene_col = table->GetColumnByName("scene");
    auto rhs_col = table->GetColumnByName("rhs");
    if (!scene_col || !rhs_col || (has_iteration_col && !iteration_col)) {
      return std::unexpected(Error {
          .code = ErrorCode::InvalidInput,
          .message =
              std::format("Boundary cuts CSV {}: missing required column "
                          "(iteration/scene/rhs)",
                          filepath),
      });
    }

    // Resolve the per-state-variable ChunkedArray pointers once; they
    // mirror ``header_col_map`` 1-to-1 (one entry per trailing state
    // header), including ``nullptr`` for headers Arrow somehow failed
    // to surface (defensive — should not happen with valid CSV).
    std::vector<std::shared_ptr<arrow::ChunkedArray>> state_cols;
    state_cols.reserve(static_cast<std::size_t>(num_state_cols));
    for (auto hi = static_cast<std::size_t>(state_var_start);
         hi < headers.size();
         ++hi)
    {
      auto col = table->GetColumnByName(headers[hi]);
      if (!col) {
        return std::unexpected(Error {
            .code = ErrorCode::InvalidInput,
            .message = std::format("Boundary cuts CSV {}: missing column "
                                   "'{}' on the Arrow Table",
                                   filepath,
                                   headers[hi]),
        });
      }
      state_cols.push_back(std::move(col));
    }

    // State-variable columns may be int32 or float64 depending on
    // input data; cast each to float64 with the Compute kernel so the
    // downstream extraction is uniform.
    auto cast_to_f64 = [](std::shared_ptr<arrow::ChunkedArray> col)
        -> arrow::Result<std::shared_ptr<arrow::ChunkedArray>>
    {
      if (col->type()->id() == arrow::Type::DOUBLE) {
        return col;
      }
      arrow::compute::CastOptions opts;
      opts.to_type = arrow::float64();
      ARROW_ASSIGN_OR_RAISE(auto result, arrow::compute::Cast(col, opts));
      return result.chunked_array();
    };
    for (auto& col : state_cols) {
      auto cast_result = cast_to_f64(col);
      if (!cast_result.ok()) {
        return std::unexpected(Error {
            .code = ErrorCode::InvalidInput,
            .message = std::format("Boundary cuts CSV {}: cannot cast a state "
                                   "column to float64: {}",
                                   filepath,
                                   cast_result.status().ToString()),
        });
      }
      col = std::move(*cast_result);
    }

    const auto nrows = table->num_rows();
    std::vector<RawBoundaryCut> raw_cuts;
    raw_cuts.reserve(static_cast<std::size_t>(nrows));

    // Arrow ChunkedArrays may be split into multiple chunks (for
    // large files); chunk-walk every column in lockstep to recover
    // the per-row tuple without forcing a Table → contiguous copy.
    int64_t row_global = 0;
    const int n_chunks = scene_col->num_chunks();
    for (int chunk_i = 0; chunk_i < n_chunks; ++chunk_i) {
      const auto scene_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
          scene_col->chunk(chunk_i));
      const auto rhs_arr = std::dynamic_pointer_cast<arrow::DoubleArray>(
          rhs_col->chunk(chunk_i));
      std::shared_ptr<arrow::Int32Array> iter_arr;
      if (iteration_col) {
        iter_arr = std::dynamic_pointer_cast<arrow::Int32Array>(
            iteration_col->chunk(chunk_i));
      }
      if (!scene_arr || !rhs_arr || (iteration_col && !iter_arr)) {
        return std::unexpected(Error {
            .code = ErrorCode::InvalidInput,
            .message = std::format("Boundary cuts CSV {}: unexpected "
                                   "column types — schema mismatch",
                                   filepath),
        });
      }

      const auto chunk_rows = scene_arr->length();
      for (int64_t i = 0; i < chunk_rows; ++i) {
        IterationIndex iteration_index {};
        if (iter_arr && iter_arr->IsValid(i)) {
          iteration_index = IterationIndex {iter_arr->Value(i)};
        }
        const SceneUid scene_uid = make_uid<Scene>(scene_arr->Value(i));
        const double rhs = rhs_arr->Value(i);

        // Pull per-state-column value; null becomes NaN so the
        // downstream "non-zero coefficient on missing column" check
        // treats it as zero (no warning, no skip) which mirrors the
        // legacy "empty token" behaviour.
        std::vector<double> coeffs;
        coeffs.reserve(state_cols.size());
        for (const auto& col : state_cols) {
          const auto chunk_arr = std::dynamic_pointer_cast<arrow::DoubleArray>(
              col->chunk(chunk_i));
          if (chunk_arr && chunk_arr->IsValid(i)) {
            coeffs.push_back(chunk_arr->Value(i));
          } else {
            coeffs.push_back(0.0);
          }
        }

        raw_cuts.push_back(RawBoundaryCut {
            .iteration_index = iteration_index,
            .scene_uid = scene_uid,
            .rhs = rhs,
            .coeffs = std::move(coeffs),
        });
        ++row_global;
      }
    }
    (void)row_global;

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
    //    install below calls `bound_alpha(…, last_phase)` to
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
    // multicut-only: PLP cuts whose source scenario UID is not in the gtopt
    // scene set (e.g. when --hydrologies selects N_gtopt < NVarPhi) have no
    // varphi_s column to land on and are dropped.  Dropping them silently
    // biases the terminal value function (the dropped scenarios' cost-to-go
    // is ignored), so we count and warn — see the lp-numerics analysis that
    // keeps `combined` (mechanism A) the plp2gtopt default.
    int multicut_cuts_dropped = 0;
    // Per-scene accumulator: collect every (scene_index → SparseRow) install
    // here, then dispatch one bulk `add_rows` per scene at the end of the
    // outer loop.  Saves ~`raw_cuts.size()` backend round-trips per scene
    // on the boundary-cut load path (which can be thousands of cuts in a
    // production recovery run).  Row construction below is unchanged —
    // only the install step moves from per-cut `add_cut_row` to the
    // collect-then-bulk pattern below.
    std::vector<std::vector<SparseRow>> scene_cuts(
        static_cast<size_t>(num_scenes));
    for (const auto& rc : raw_cuts) {
      // Pre-scan: check for missing state variables with non-zero
      // coefficients.  Reads from the pre-decoded ``rc.coeffs`` vector
      // built from the Arrow Table — no more on-the-fly tokenisation.
      bool has_missing = false;
      {
        const auto ncoeffs = std::min(header_col_map.size(), rc.coeffs.size());
        for (std::size_t ci = 0; ci < ncoeffs; ++ci) {
          const double v = rc.coeffs[ci];
          if (!header_col_map[ci].has_value() && v != 0.0) {
            has_missing = true;
            if (!warned_missing_cols.contains(ci)) {
              warned_missing_cols.insert(ci);
              SPDLOG_WARN(
                  "Boundary cuts: state variable '{}' not "
                  "found in the current model; non-zero "
                  "coefficient {} (mode={})",
                  header_names[ci],
                  v,
                  enum_name(missing_mode));
            }
          }
        }
      }
      if (has_missing && missing_mode == MissingCutVarMode::skip_cut) {
        ++cuts_skipped;
        continue;
      }

      // Determine which scenes get this cut, and which α column it targets.
      //   * separated (per_scene): install only on the matching scene's α.
      //   * combined  (shared):    broadcast onto every scene's single α.
      //   * multicut:               broadcast onto EVERY scene's terminal LP
      //     but target `varphi_{source}` (the α dedicated to the cut's source
      //     scenario), never the destination's own α — keeping each
      //     scenario's terminal cuts on a dedicated column.
      SceneIndex scene_start {0};
      SceneIndex scene_end {num_scenes};
      std::optional<SceneIndex> source_scene;
      if (separated || multicut) {
        auto it = scene_uid_to_index.find(rc.scene_uid);
        if (it == scene_uid_to_index.end()) {
          // Spdlog formats the structured cut-identity tuple
          // directly — no pre-baked name string.
          SPDLOG_TRACE(
              "Boundary cut (iter={}, scene_uid={}) not found in "
              "scene_array -- skipping",
              uid_of(rc.iteration_index),
              rc.scene_uid);
          if (multicut) {
            ++multicut_cuts_dropped;
          }
          continue;
        }
        if (multicut) {
          // Broadcast to all scenes; target this scenario's own varphi_s.
          source_scene = it->second;
        } else {
          scene_start = it->second;
          scene_end = next(it->second);
        }
      }

      for (const auto scene_index : iota_range(scene_start, scene_end)) {
        const auto* alpha_svar = source_scene
            ? find_alpha_state_var(planning_lp.simulation(),
                                   scene_index,
                                   last_phase,
                                   *source_scene)
            : find_alpha_state_var(
                  planning_lp.simulation(), scene_index, last_phase);
        if (alpha_svar == nullptr) {
          continue;  // No α on this (scene, phase) — nothing to add
        }

        // IterationContext carries a per-cut (iter, offset) pair
        // so multiple boundary cuts for the same (scene, phase)
        // don't collide on the metadata-based duplicate detector
        // in `LinearInterface::add_row`.  The iteration is read
        // from the optional ``iteration`` CSV column (``rc.iteration_index``);
        // ``cuts_loaded`` breaks ties when the same iteration emits
        // multiple cuts.
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
            .variable_uid = sim.uid_of(last_phase),
            // ``rc.iteration_index`` is populated by the CSV parser
            // above directly from the ``iteration`` column.
            .context = make_iteration_context(sim.uid_of(scene_index),
                                              sim.uid_of(last_phase),
                                              uid_of(rc.iteration_index),
                                              cuts_loaded),
        };
        // CutTag refactor (origin/cleanup/sddp-cut-tag-magic-strings)
        // — bundles `class_name = "Boundary"` + `constraint_name = "cut"`
        // into a single fluent `apply_to(row)` call, so the row's
        // metadata stays in lock-step if either name is later renamed.
        sddp_boundary_cut_tag.apply_to(row);
        // α physical coefficient = 1.0 (canonical optimality form
        // ``α ≥ Σᵢ rcᵢ · xᵢ + b``).  ``compose_physical`` step 1
        // multiplies by ``col_scales[α] = scale_alpha`` and step 1b
        // divides by ``scale_obj``; the resulting LP-space coefficient
        // matches the `build_benders_cut_physical` reference path.
        // (Row's default `.scale = 1.0` keeps the row in pure physical
        // units — `compose_physical` applies col_scale + ÷scale_obj
        // exactly once.  See P0-1 fix in commit eb7bc1f0.)
        row[alpha_svar->col()] = 1.0;

        const auto ncoeffs = std::min(header_col_map.size(), rc.coeffs.size());
        for (std::size_t ci = 0; ci < ncoeffs; ++ci) {
          const auto& col_opt = header_col_map[ci];
          if (!col_opt.has_value()) {
            continue;  // skip_coeff: drop missing coefficient
          }
          const auto coeff = rc.coeffs[ci];
          if (coeff != 0.0) {
            // State-variable physical coefficient in $/(physical-unit).
            // `compose_physical` applies the column scale itself.
            row[*col_opt] = -coeff * bc_discount;
          }
        }

        // Boundary cuts encode the future-cost function at the
        // horizon (optimality-style: α ≥ state-dependent RHS on the
        // last phase).  Each row always carries α (set above via
        // `row[alpha_svar->col()] = 1.0`), so the Optimality gate in
        // `bound_alpha_for_cut` releases the bootstrap pin during the
        // batched install pass below.  Pass `cut_coeff_eps` so loaded
        // cuts share the same numerical-noise filter that the SDDP
        // backward-pass uses for generated cuts (audit P2-1) — without
        // it, the install path defaulted to ``eps=0`` and small
        // physical coefficients (e.g., ill-conditioned PLP exports)
        // could survive into the LP and degrade kappa.
        scene_cuts[static_cast<size_t>(scene_index)].push_back(std::move(row));
      }
      max_iteration = std::max(max_iteration, rc.iteration_index);
      ++cuts_loaded;
    }

    // ── α-rebase (mean-shift): zero-mean cut RHSs via obj_constant ──
    //
    // Opt-in via `options.boundary_cuts_mean_shift` (default off).
    // Disabled-path leaves `scene_c_bar` all-zero, so the CALLER's
    // `add_obj_constant` restitution is skipped entirely — the
    // pre-existing cut RHS magnitudes survive byte-identical and
    // every test that asserts on raw on-disk RHS continues to pass.
    //
    // Enabled-path is an algebraically equivalent rewrite that
    // lets the LP-side α variable be centred around 0 instead of
    // carrying the boundary cuts' typical multi-million-dollar
    // terminal-value baseline.  Definition: α' = α − c̄ where c̄ is
    // the per-scene mean of installed cut RHSs (post-`bc_discount`,
    // same units as `row.lowb`).
    //
    // For each scene with at least one cut, this loader:
    //   1. computes c̄_scene = mean(cut.lowb over cuts on this scene),
    //   2. subtracts c̄_scene from every cut.lowb on the scene,
    //   3. returns c̄_scene in `alpha_offsets_per_scene`.
    // The CALLER then restores the algebraically-original objective by
    // handing c̄ to the solver's NATIVE objective offset via
    // `LinearInterface::add_obj_constant`: `MonolithicMethod` folds it into
    // the single scene LP (last phase = whole LP), and `SDDPMethod` folds it
    // into each scene's FIRST-phase master LP (see those files for why the
    // first phase is the correct fold site under the SDDP decomposition).
    //
    // Effects / properties (enabled):
    //   * Cut RHS range is unchanged (mean shift, not rescale), but
    //     values centre on zero — LP equilibration sees comparable
    //     RHS magnitudes for boundary vs. runtime cuts.
    //   * Subsequent backward-pass cuts compose naturally — they're
    //     computed from the shifted LP, so their intercepts already
    //     live in the same basis.
    //   * The α pin (`lowb = uppb = 0` from
    //     `register_alpha_variables`) does NOT need to change: the
    //     first cut install releases α to `(−∞, +∞)`, where α can
    //     be negative.
    //   * c̄ = 0 (no cuts on a scene, or already zero-mean) is a
    //     no-op shift — the caller's `add_obj_constant(0)` is harmless.
    //   * Loaded cut files round-trip correctly: c̄ is recomputed
    //     from the on-disk RHSs each time, and the recomputed value
    //     matches the original because the load is deterministic.
    const bool mean_shift = options.boundary_cuts_mean_shift;
    StrongIndexVector<SceneIndex, double> scene_c_bar(num_scenes, 0.0);
    if (mean_shift) {
      // ── State-var column → final-target value (efin / eini) ─────────
      // The α-rebase offset `c` is the cut value at the *expected
      // satisfied* end-state, which mirrors the Python oracle
      // (`build_fcf_alpha_terms`: `c = FCF − Σ wv·efin`).  Evaluating the
      // cut at the reservoir's `efin` end-of-horizon target — rather than
      // the bound-box midpoint — gives the least objective perturbation
      // when the efin target is met, and keeps the LP-side α' centred on
      // ~0 at the solution.  Fall back to `eini`, then the LP-bound
      // midpoint (= (emin+emax)/2 in physical units), when `efin` is
      // absent.  NOTE: we only *read* the existing `efin`; we never set
      // `efin = eini`.
      //
      // The map keys the last-phase state-var LP column to its target so
      // the per-cut loop can prefer it over the midpoint per coefficient.
      std::unordered_map<ColIndex, double> col_target;
      map_reserve(col_target,
                  sys.reservoir_array.size() + sys.battery_array.size());
      const auto add_target = [&](std::string_view cname,
                                  Uid elem_uid,
                                  std::optional<double> target)
      {
        if (!target.has_value()) {
          return;
        }
        for (const auto& [key, svar] : svar_map) {
          if (key.uid == elem_uid && key.class_name == cname
              && is_final_state_col(key.col_name))
          {
            col_target[svar.col()] = *target;
            break;
          }
        }
      };
      for (const auto& rsv : sys.reservoir_array) {
        // efin first, then eini; never assign efin = eini (read-only).
        const auto target = rsv.efin.has_value() ? rsv.efin : rsv.eini;
        add_target("Reservoir", rsv.uid, target);
      }
      for (const auto& bat : sys.battery_array) {
        // Batteries have no end-of-horizon target field; eini is the
        // closest analogue (initial state of charge).  Midpoint fallback
        // otherwise.
        add_target("Battery", bat.uid, bat.eini);
      }

      // c̄_scene is the mean of each cut's value AT THE MIDPOINT
      // STATE — i.e., `rhs + ⟨g, midpoint⟩` where `midpoint_i =
      // ½ (lowb_i + uppb_i)` for each state variable `i`.  Equivalent
      // to the "expected α value" if every state variable were
      // uniformly distributed on its physical bound box.  Subtracting
      // this from each cut's `lowb` centres the LP-side α' at
      // (approximately) zero across the polyhedral region the
      // backward pass is likely to visit, which is sharper than the
      // simple `mean(rhs)` for cuts with non-zero state coefficients.
      //
      // Cut row sign convention: rows are `α + Σ (−gᵢ) sᵢ ≥ rhs`, so
      // `cut.cmap[sᵢ] = −gᵢ`.  The cut's value at the midpoint is
      // `rhs + Σ gᵢ × midᵢ = rhs − Σ cut.cmap[sᵢ] × midᵢ` — the
      // subtraction below absorbs the `−1` factor.
      //
      // Unbounded state variables: when any non-α coefficient touches
      // a state with an unbounded raw bound (±infinity), the midpoint
      // is undefined and the cut is skipped from the average — same
      // safety gate as `apply_alpha_floor` in
      // `source/sddp_method_alpha.cpp`.  Cuts that survive the gate
      // are still installed; only their contribution to c̄ is dropped.
      for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
        if (cuts.empty()) {
          continue;
        }
        const auto* alpha_svar = find_alpha_state_var(sim, si, last_phase);
        if (alpha_svar == nullptr) {
          continue;  // no α on this (scene, phase) → no shift
        }
        const auto alpha_col = alpha_svar->col();
        auto& li = planning_lp.system(si, last_phase).linear_interface();
        const auto col_low_phys = li.get_col_low();
        const auto col_upp_phys = li.get_col_upp();
        const auto col_low_raw = li.get_col_low_raw();
        const auto col_upp_raw = li.get_col_upp_raw();

        double sum_at_mid = 0.0;
        int n_finite = 0;
        for (const auto& cut : cuts) {
          double cut_at_mid = cut.lowb;
          bool unbounded = false;
          for (const auto& [col, coef] : cut.cmap) {
            if (col == alpha_col) {
              continue;
            }
            if (coef == 0.0) {
              continue;
            }
            const auto idx = static_cast<std::size_t>(col);
            if (li.is_pos_inf(col_upp_raw[idx])
                || li.is_neg_inf(col_low_raw[idx]))
            {
              unbounded = true;
              break;
            }
            // Prefer the reservoir/battery final-target value (efin /
            // eini) when available — the cut value there is `c`, the
            // cost-to-go at the expected satisfied end-state (mirrors the
            // Python oracle `c = FCF − Σ wv·efin`).  Fall back to the
            // bound-box midpoint when no target is registered for this
            // column.
            const auto target_it = col_target.find(col);
            const double eval_point = (target_it != col_target.end())
                ? target_it->second
                : 0.5 * (col_low_phys[col] + col_upp_phys[col]);
            // cmap holds −gᵢ; the cut value at `eval_point` is
            //   `rhs + ⟨g, eval⟩ = rhs − Σ cmap[sᵢ] × evalᵢ`.
            cut_at_mid -= coef * eval_point;
          }
          if (unbounded) {
            continue;
          }
          sum_at_mid += cut_at_mid;
          ++n_finite;
        }
        if (n_finite > 0) {
          const double c_bar = sum_at_mid / static_cast<double>(n_finite);
          // α' = α − c̄ is a clean change of variable.  α is
          // registered (`sddp_method_alpha.cpp:107`) with
          // `cost = 1.0` (physical $), so the obj_value drop from
          // the substitution is exactly `−c̄` per scene — no
          // `cost_factor` (= prob × disc × dur) multiplier.  The raw
          // `c̄` is returned to the caller (`SDDPMethod`), which
          // restores the algebraically-original objective by folding
          // it into the first-phase master LP's NATIVE objective
          // offset via `add_obj_constant` — so `get_obj_value()` (and
          // thus every UB/LB site) already carries it; no display
          // add-back remains.
          //
          // History: commit `ccd2833c` (2026-05-16) pre-multiplied
          // by `cf` claiming "α's coefficient is `cf`, not 1.0".
          // That misread the α column registration — α's obj
          // coefficient really is 1.0 (per-period costs are baked
          // into other cols' coefficients via `block_ecost`, not
          // into α's).  Reverted 2026-05-17 to the initial
          // (`f9181432`) form.
          scene_c_bar[si] = c_bar;
          if (c_bar != 0.0) {
            for (auto& cut : cuts) {
              cut.lowb -= c_bar;
            }
          }
        }
      }
    }

    // Per-scene install through the unified `gtopt::add_cut_row` entry
    // (sddp_types.hpp) — same path that runtime-generated optimality
    // cuts take in `SDDPMethod::backward_pass_single_phase`.  That
    // single call handles, per cut, in this order:
    //
    //   1. α-release via `bound_alpha_for_cut` when the cut references
    //      α (boundary cuts always do — `row[alpha_svar->col()] = 1.0`
    //      is set in the build loop above).  Idempotent across the
    //      batch, so a redundant release across N cuts is a cheap
    //      `set_col_low_raw / set_col_upp_raw` no-op.
    //   2. `LinearInterface::add_cut_row` which `add_row`s the cut
    //      AND `record_cut_row`s it.  Post the `6cf57176` fix this
    //      tracks under every `LowMemoryMode` (including `off`), so
    //      every `clone_from_flat(with_replay=true)` produced AFTER
    //      `freeze_for_cuts` will see the boundary cuts re-applied
    //      from `m_replay_.active_cuts()` — load_flat alone would
    //      load only the pre-snapshot structural rows.
    //
    // Using the unified path here (rather than the previous bulk
    // add_rows + manual record_cut_row split) keeps the boundary-cut
    // install semantically identical to the runtime cut install, so a
    // future change to `add_cut_row` propagates here automatically and
    // there is one canonical sequence of "release → add → record" that
    // every cut on α follows.  The N≈1k per-cut calls at startup are
    // dwarfed by even one SDDP iteration's solve time.
    //
    // FutureCost.single_cut_equality (default true): when a scene has
    // exactly ONE boundary cut, install it as the EQUALITY ``α + Σ
    // wvᵣ·efinᵣ = FCF`` with α freed (the continuous PLEXOS-style terminal
    // value).  When the active FutureCost element sets it ``false`` the lone
    // cut stays the slack-able ``≥`` Benders lower-bound form (α NOT freed)
    // — used for a faithful comparison against a user-authored ``≥`` cut.
    // No FutureCost element / unset ⇒ ``value_or(true)`` ⇒ legacy behaviour.
    const auto* fc = gtopt::active_future_cost(planning_lp);
    const bool single_cut_equality_opt =
        (fc == nullptr) || fc->single_cut_equality.value_or(true);

    for (auto&& [si, cuts] : enumerate<SceneIndex>(scene_cuts)) {
      if (cuts.empty()) {
        continue;
      }
      // Single linear terminal value (the typical PLP / PLEXOS boundary
      // cut, one row per scene): install it as an EQUALITY and free α, so
      // the cut becomes the *continuous* terminal value (PLEXOS-style
      // ``+ Σ wvᵣ·efinᵣ``) rather than a slack-able ``≥`` lower bound that
      // is ignored above the efin target.  With ``α + Σ wvᵣ·efinᵣ = FCF``
      // and α free, α is pinned to ``FCF − Σ wvᵣ·efinᵣ``, so its dual is 1
      // and every reservoir's terminal storage is priced at ``wvᵣ`` on
      // both sides of the target — matching how PLEXOS applies its single
      // MT water value.  With MANY cuts (a piecewise-linear future cost)
      // we keep the ``≥`` Benders form (the value function is the max of
      // its supporting cuts) regardless of single_cut_equality.
      const bool as_equality = (cuts.size() == 1) && single_cut_equality_opt;
      for (auto& cut : cuts) {
        if (as_equality) {
          cut.uppb = cut.lowb;  // ``≥``  →  ``=``
        }
        (void)gtopt::add_cut_row(planning_lp,
                                 si,
                                 last_phase,
                                 CutType::Optimality,
                                 cut,
                                 options.cut_coeff_eps);
      }
      if (as_equality) {
        // Free α (override the cut-derived floor `bound_alpha_for_cut`
        // installs) so the equality is feasible when Σ wvᵣ·efinᵣ > FCF —
        // α must be able to go negative for the reward to keep applying as
        // the reservoir conserves above the target.
        const auto* asv = find_alpha_state_var(sim, si, last_phase);
        if (asv != nullptr) {
          auto& li = planning_lp.system(si, last_phase).linear_interface();
          li.set_col_low_raw(asv->col(), -li.infinity());
        }
      }
      // Log the per-scene mean-shift (the shift itself was applied
      // to `cut.lowb` in the pre-install pass above).  The c̄
      // offsets are returned in `CutLoadResult`; `SDDPMethod` folds
      // them into the first-phase master LP's native objective offset
      // via `add_obj_constant`, so the reported UB / LB carry the
      // pre-shift physical objective without any display add-back.
      const double c_bar = scene_c_bar[si];
      if (c_bar != 0.0) {
        SPDLOG_INFO(
            "SDDP boundary cuts: α-rebase scene_uid={} c̄={:.6e} "
            "({} cuts shifted)",
            sim.uid_of(si),
            c_bar,
            cuts.size());
      }
      // Terminal α stays free (no derived floor).  An earlier version
      // pinned α_T's column at a cut-derived lower-bound floor to
      // close a `CPX_STAT_UNBOUNDED` window on aperture clones with
      // perturbed trial states; under mean-shift the floor was
      // conflated with α's natural movement, so we drop the pin
      // entirely.  α_T behaves like any other α_t — bounded only by
      // its cuts.  Re-add the floor here if `CPX_STAT_UNBOUNDED`
      // resurfaces at the terminal phase under aperture perturbations.
    }

    if (cuts_skipped > 0) {
      SPDLOG_WARN(
          "Boundary cuts: skipped {} cut(s) referencing "
          "missing state variables",
          cuts_skipped);
    }

    if (multicut_cuts_dropped > 0) {
      SPDLOG_WARN(
          "SDDP multicut boundary: dropped {} cut(s) whose source scenario "
          "UID is not in the gtopt scene_array (N_scenes < NVarPhi).  The "
          "terminal value function ignores those scenarios' cost-to-go, "
          "biasing LB downward.  Prefer boundary_cuts_mode=combined "
          "(per_scene α) when the scene set is a subset of the PLP "
          "hydrologies.",
          multicut_cuts_dropped);
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
        // Return the per-scene α-rebase offsets (zero-filled when the
        // shift is disabled).  SDDPMethod folds these into the first-phase
        // master LP's native objective offset (`add_obj_constant`) so the
        // user sees physical (pre-shift) UB / LB values regardless of how
        // the cuts are stored internally on the LP.
        .alpha_offsets_per_scene = std::move(scene_c_bar),
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
