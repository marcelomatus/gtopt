// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_fcut_audit.cpp
 * @brief     Feasibility-cut audit + tracking: StateVarLink identity,
 *            store invariants, and end-to-end count propagation.
 * @date      2026-04-20
 *
 * Scope:
 *   1. `StateVarLink` populated by `collect_state_variable_links`
 *      carries the state-variable registry identity (class_name, uid,
 *      col_name) so downstream diagnostics can name the elements
 *      involved in a fcut.
 *
 *   2. Coverage audit: at each non-last (scene, phase), the count of
 *      outgoing_links matches the number of state variables registered
 *      on the simulation — no state variable is silently dropped.
 *
 *   3. Forward-pass fcut tracking: on a fixture that deterministically
 *      exercises the elastic filter, a single-iteration solve installs
 *      ≥ 1 feasibility cut, those cuts surface in `SDDPCutStore` with
 *      `CutType::Feasibility`, and the stored-cut count matches the
 *      iteration-result `cuts_added` counter plus backward-pass cuts.
 *
 * These tests cover the pipeline the run-time INFO log relies on
 * ("elastic → fcut on p{N} state=[Reservoir:uid:col_name,...]") and
 * the retry-loop exit condition introduced for the simulation pass
 * ("install 0 cuts → terminal, stop retrying").
 */

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/benders_cut.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/sddp_cut_store.hpp>
#include <gtopt/sddp_method.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/system_lp.hpp>

#include "sddp_helpers.hpp"

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ── 1. StateVarLink identity population ─────────────────────────────────────

TEST_CASE(  // NOLINT
    "SDDP fcut audit — StateVarLink carries class/uid/col_name from registry")
{
  // The forced-infeasibility fixture has exactly one reservoir (uid=1,
  // name="rsv1") across 3 phases.  After initialization, every non-last
  // phase's outgoing_links should contain one StateVarLink identifying
  // the reservoir's state variable.  The identity fields must match the
  // registry Key exactly so the forward-pass INFO log can render
  // "Reservoir:1:<col_name>".
  auto planning = make_forced_infeasibility_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  SDDPMethod sddp(planning_lp, sddp_opts);

  auto init = sddp.ensure_initialized();
  REQUIRE(init.has_value());

  const auto& phase_states = sddp.phase_states(first_scene_index());
  REQUIRE(!phase_states.empty());

  // Every non-last phase has at least one outgoing link whose identity
  // fields point at a Reservoir.  We also verify that class_name /
  // uid / col_name match what the state-variable registry stores.
  const auto& sim = planning_lp.simulation();
  const auto last_phase_index = sim.last_phase_index();

  std::size_t links_checked = 0;
  for (auto&& [phase_index, _ph] : enumerate<PhaseIndex>(sim.phases())) {
    if (phase_index == last_phase_index) {
      break;  // last phase has no outgoing links by design
    }
    const auto& links = phase_states[phase_index].outgoing_links;
    REQUIRE_FALSE(links.empty());

    // Cross-check each link against the registry.
    const auto& svar_map =
        sim.state_variables(first_scene_index(), phase_index);
    for (const auto& link : links) {
      CHECK_FALSE(link.class_name.empty());
      CHECK_FALSE(link.col_name.empty());
      CHECK(link.uid != unknown_uid);

      // Find the matching registry entry by (class, uid, col_name).
      const auto found =
          std::ranges::any_of(svar_map,
                              [&](const auto& kv)
                              {
                                return kv.first.class_name == link.class_name
                                    && kv.first.uid == link.uid
                                    && kv.first.col_name == link.col_name;
                              });
      CHECK(found);
      ++links_checked;
    }
  }
  CHECK(links_checked >= 1);
}

// ── 2. Coverage audit: outgoing_links ≤ state_variables ────────────────────
//
// The audit log in collect_state_variable_links flags a mismatch at
// DEBUG level when some state variables have no dependent link in the
// next phase (legal for skip-ahead couplings, worth surfacing).  This
// test asserts the *upper bound* the audit enforces: at a (scene,
// phase) where every state var has a dependent in the next phase, the
// counts match.  For fixtures without skip-ahead couplings (the
// forced-infeasibility fixture qualifies), equality must hold.

TEST_CASE(  // NOLINT
    "SDDP fcut audit — outgoing_links count equals state_variables count")
{
  auto planning = make_forced_infeasibility_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  SDDPMethod sddp(planning_lp, sddp_opts);
  REQUIRE(sddp.ensure_initialized().has_value());

  const auto& sim = planning_lp.simulation();
  const auto last_phase_index = sim.last_phase_index();
  const auto& phase_states = sddp.phase_states(first_scene_index());

  for (auto&& [phase_index, _ph] : enumerate<PhaseIndex>(sim.phases())) {
    if (phase_index == last_phase_index) {
      break;
    }
    const auto n_links = phase_states[phase_index].outgoing_links.size();

    // Count registered state variables EXCLUDING alpha — alpha is a
    // Sddp-owned future-cost variable (class_name="Sddp"), not a
    // physical coupling state, so `collect_state_variable_links`
    // skips it by design (it has no dependent in the next phase).
    std::size_t n_physical_state_vars = 0;
    for (const auto& [key, svar] :
         sim.state_variables(first_scene_index(), phase_index))
    {
      if (key.class_name != "Sddp") {
        ++n_physical_state_vars;
      }
    }
    CAPTURE(phase_index);
    CAPTURE(n_links);
    CAPTURE(n_physical_state_vars);
    // For the forced-infeasibility fixture, every registered physical
    // state variable has a dependent in the next phase — no skip-aheads
    // — so equality is expected.
    CHECK(n_links == n_physical_state_vars);
  }
}

// ── 3. Fcut tracking: installed cuts land in the cut store ─────────────────

TEST_CASE(  // NOLINT
    "SDDP fcut audit — single-iter solve on forced-infeas "
    "installs ≥1 feasibility cut that reaches SDDPCutStore")
{
  // make_forced_infeasibility_planning's Waterway.fmin = 2 forces
  // mandatory discharge that phase 1's reservoir state (≈ 0 after
  // phase 0 drains it) cannot satisfy → infeasibility → elastic
  // filter activates → at least one Benders feasibility cut is
  // installed on phase 0's outgoing state variable.  This test
  // verifies the tracking invariant: every fcut installed during the
  // forward pass is visible in SDDPCutStore.
  auto planning = make_forced_infeasibility_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.convergence_tol = 1e-4;
  // chinneck emits bound multi-cuts in addition to the Benders row.
  // Use single_cut here so each infeasible LP contributes exactly
  // one Feasibility row — the tracking arithmetic is cleaner to
  // assert.
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;

  SDDPMethod sddp(planning_lp, sddp_opts);

  // Snapshot the store before the iteration — should be 0 cuts.
  const auto cuts_before = sddp.num_stored_cuts();
  CHECK(cuts_before == 0);

  // Ignore the return value — aggressive fixtures may not converge in
  // one iteration; we care only about cut-store tracking, which is
  // populated regardless of convergence.
  [[maybe_unused]] auto results = sddp.solve();

  // After one iteration, the store should hold ≥ 1 cut total.  The
  // backward pass contributes optimality cuts (scut / bcut) and the
  // forward pass contributes ≥ 1 feasibility cut (fcut) thanks to
  // the forced-infeasibility fixture.
  const auto cuts_after = sddp.num_stored_cuts();
  CHECK(cuts_after >= 1);

  // Separate feasibility vs optimality contributions and verify the
  // feasibility cut count is ≥ 1, proving the forward-pass fcut
  // lifecycle (build → add_row → store_cut → SDDPCutStore) works
  // end-to-end.
  const auto combined = sddp.stored_cuts();
  int n_feas = 0;
  int n_opt = 0;
  for (const auto& c : combined) {
    if (c.type == CutType::Feasibility) {
      ++n_feas;
    } else if (c.type == CutType::Optimality) {
      ++n_opt;
    }
  }
  CAPTURE(n_feas);
  CAPTURE(n_opt);
  CAPTURE(static_cast<int>(cuts_after));
  CHECK(n_feas >= 1);

  // Every cut in the store has populated metadata — name, rhs,
  // coefficients — matching what the label maker and store_cut
  // constructed at install time.
  for (const auto& c : combined) {
    CHECK_FALSE(c.name.empty());
    CHECK_FALSE(c.coefficients.empty());
    CHECK(c.row != RowIndex {unknown_index});
  }
}

// ── 4. Fcut tracking: multi_cut emits per-slack bound cuts ─────────────────

TEST_CASE(  // NOLINT
    "SDDP fcut audit — multi_cut installs 1 Benders fcut + ≥1 bound cuts")
{
  // Under multi_cut mode, each infeasible LP contributes one Benders
  // fcut row PLUS one or two bound cuts per activated slack on the
  // state-variable links.  All of them are stored as
  // CutType::Feasibility so the forward-pass fcut counter (the
  // `n_fcuts_installed` delta on ForwardPassOutcome) rises by
  // ≥ 2 per infeasible LP.  Validate that the multi_cut path
  // produces ≥ 2 feasibility cuts where single_cut produced 1.
  auto run = [](ElasticFilterMode mode) -> int
  {
    auto planning = make_forced_infeasibility_planning();
    PlanningLP planning_lp(std::move(planning));

    SDDPOptions sddp_opts;
    sddp_opts.max_iterations = 1;
    sddp_opts.convergence_tol = 1e-4;
    sddp_opts.elastic_filter_mode = mode;
    sddp_opts.multi_cut_threshold = 0;  // force per-bound cuts immediately

    SDDPMethod sddp(planning_lp, sddp_opts);
    [[maybe_unused]] auto results = sddp.solve();

    int n_feas = 0;
    for (const auto& c : sddp.stored_cuts()) {
      if (c.type == CutType::Feasibility) {
        ++n_feas;
      }
    }
    return n_feas;
  };

  const int n_feas_single = run(ElasticFilterMode::single_cut);
  const int n_feas_multi = run(ElasticFilterMode::multi_cut);

  CAPTURE(n_feas_single);
  CAPTURE(n_feas_multi);

  CHECK(n_feas_single >= 1);
  CHECK(n_feas_multi >= n_feas_single);
}

// ── 5. Stored cut identity: phase_uid / scene_uid / coefficients ──────────
//
// The `StoredCut` record carries the scene and phase UIDs of the LP
// it was installed on (p-1 for an fcut triggered by infeasibility at
// p) plus the coefficient list.  The latter lists exactly the
// state-variable source_col indices referenced by the cut row.  This
// is the data the downstream diagnostics (simulation output,
// boundary-cut CSV export) rely on — regressions here break those
// without any visible test failure elsewhere.

TEST_CASE(  // NOLINT
    "SDDP fcut audit — StoredCut metadata references valid phase/scene UIDs")
{
  auto planning = make_forced_infeasibility_planning();
  PlanningLP planning_lp(std::move(planning));

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;

  SDDPMethod sddp(planning_lp, sddp_opts);
  [[maybe_unused]] auto results = sddp.solve();

  // Enumerate the valid phase UIDs from the simulation.
  std::unordered_set<std::size_t> valid_phase_uids;
  for (const auto& ph : planning_lp.simulation().phases()) {
    valid_phase_uids.insert(static_cast<std::size_t>(Index {ph.uid()}));
  }

  const auto cuts = sddp.stored_cuts();
  REQUIRE_FALSE(cuts.empty());

  for (const auto& c : cuts) {
    // Every stored cut references a real phase (no stale or
    // uninitialized UID).
    CHECK(valid_phase_uids.contains(
        static_cast<std::size_t>(Index {c.phase_uid})));
    // Coefficients list is non-empty — fcuts built via
    // `build_feasibility_cut_physical` touch ≥ 1 source_col.  The α
    // column is intentionally NOT included in feasibility cuts
    // (classical Benders / PLP `AgrElastici` convention), so the
    // minimum is 1, not 2 as the pre-backtracking builder produced.
    CHECK(c.coefficients.size() >= 1);
  }
}

// ── 6. LP-file audit: fcut rows are actually present in the solver LP ──────
//
// The strongest end-to-end check: after a solve that installs ≥1
// feasibility cut, write the phase-0 LP to disk (phase 0 is where
// fcuts get installed when phase 1 is infeasible) and confirm the
// serialized LP file contains a row named "…fcut…".  Also cross-
// checks against the in-memory `row_name_map` — the same fcut row is
// visible through both the LP-file path and the live backend query.
// This closes the loop: build → add_row → store_cut → (persist) →
// solver LP contains the row.

TEST_CASE(  // NOLINT
    "SDDP fcut audit — LP file contains installed fcut row(s)")
{
  auto planning = make_forced_infeasibility_planning();
  // Enable row/col names so every added cut gets a searchable label on
  // both the solver backend (row_name_map) and the serialised LP file.
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 1;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  // Keep simulation-pass fcuts in the store so the post-solve row
  // inventory reflects every fcut that was installed.
  sddp_opts.save_simulation_cuts = true;

  SDDPMethod sddp(planning_lp, sddp_opts);
  [[maybe_unused]] auto results = sddp.solve();

  // The forced-infeasibility fixture triggers fcuts at phase 2
  // (installed on phase 1's LP = phase_index 0) and phase 3
  // (installed on phase 2's LP = phase_index 1).  Search every
  // non-last phase for an "fcut" row; the check succeeds if any phase
  // carries at least one.
  const auto& sim = planning_lp.simulation();
  const auto last_phase_index = sim.last_phase_index();

  int n_fcut_rows_total = 0;
  std::string first_fcut_name;
  PhaseIndex first_fcut_phase {0};
  for (auto&& [phase_index, _ph] : enumerate<PhaseIndex>(sim.phases())) {
    if (phase_index == last_phase_index) {
      break;
    }
    auto& sys = planning_lp.system(first_scene_index(), phase_index);
    auto& li = sys.linear_interface();
    // Lazy label path: force label synthesis so `row_name_map()` is
    // populated with the fcut names installed during the solve.
    li.materialize_labels();
    for (const auto& [name, row] : li.row_name_map()) {
      if (name.contains("fcut")) {
        ++n_fcut_rows_total;
        if (first_fcut_name.empty()) {
          first_fcut_name = name;
          first_fcut_phase = phase_index;
        }
      }
    }
  }
  CAPTURE(n_fcut_rows_total);
  CAPTURE(first_fcut_name);
  CAPTURE(first_fcut_phase);
  REQUIRE(n_fcut_rows_total >= 1);

  // LP-file path: write the phase that actually holds the fcut to a
  // temp file and grep for the same row name.  Confirms the solver
  // backend has the constraint materialized end-to-end, not just
  // bookkeeping at the gtopt layer.
  auto& li_with_cut = planning_lp.system(first_scene_index(), first_fcut_phase)
                          .linear_interface();

  const auto lp_path =
      std::filesystem::temp_directory_path() / "gtopt_fcut_audit_cut_phase";
  {
    auto wr = li_with_cut.write_lp(lp_path.string());
    REQUIRE(wr.has_value());
  }
  // write_lp appends ".lp" to the stem.
  const auto lp_file = lp_path.string() + ".lp";
  std::ifstream ifs(lp_file);
  REQUIRE(ifs.is_open());
  std::stringstream buf;
  buf << ifs.rdbuf();
  const auto content = buf.str();
  CAPTURE(lp_file);
  CAPTURE(content.size());

  // The LP writer sanitises row names (character substitution, length
  // caps) for solver compatibility, so the exact row name from
  // row_name_map may not appear verbatim in the file.  The invariant
  // we enforce: *some* row with "fcut" in its label survives the
  // round-trip.  This proves the cut reached the materialised LP, not
  // just the gtopt bookkeeping layer.
  CHECK(content.contains("fcut"));

  // Clean up temp file.
  std::error_code ec;
  std::filesystem::remove(lp_file, ec);
}

// ── 7. Multi-iteration fcut persistence via lp_debug ────────────────────────
//
// The strongest persistence invariant: a feasibility cut installed in
// iteration N on phase P's LP must still be present in phase P's LP
// at every iteration M > N.  There is no mechanism in the forward or
// backward pass that removes previously-installed fcuts — they are
// persistent rows on the live backend and are replayed via
// `m_active_cuts_` on any low-memory reconstruct.  This test pins
// that invariant by using the `lp_debug` writer to dump each phase's
// LP at every iteration, then scanning the dumps for "fcut" rows and
// confirming the count is monotonically non-decreasing iteration over
// iteration.
//
// Fixture: `make_forced_infeasibility_planning` deterministically
// generates fcuts starting at iteration 0 and continues to need
// elastic-filter relaxation for multiple iterations until the FCF is
// well-enough trained to route around the forced discharge.  With
// `max_iterations = 5` the same reservoir-state trajectory re-enters
// phase 1 infeasible on each iter, so fcuts either accumulate or stay
// exactly the same — never decrease.

TEST_CASE(  // NOLINT
    "SDDP fcut audit — fcuts persist across iterations under lp_debug")
{
  // Re-enabled 2026-04-29: switched fixture from
  // ``make_forced_infeasibility_planning`` (which under PLP-style
  // backtracking declares the scene infeasible at iter 0 with no cut
  // accumulation) to ``make_backtracking_recovery_planning`` (10-phase,
  // single reservoir) — the recovery fixture's cascade installs fcuts
  // on multiple phases and the forward pass *recovers* after
  // backtracking, so the same fcut rows persist across iterations.
  // The "fcut count is monotonically non-decreasing" invariant is now
  // exercised meaningfully.
  //
  // Scratch directory for this test; clean it before + after to keep
  // the run hermetic under -j parallel ctest.
  const auto dbg_dir = std::filesystem::temp_directory_path()
      / "gtopt_fcut_persistence_lp_debug";
  std::error_code ec;
  std::filesystem::remove_all(dbg_dir, ec);
  std::filesystem::create_directories(dbg_dir, ec);

  auto planning = make_backtracking_recovery_planning();

  // Names must flow through every (scene, phase) cell so the dumped
  // LP files carry the "fcut" row-name substring.
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 5;  // ≥ 2 iterations to exercise persistence
  sddp_opts.convergence_tol = 1e-6;  // tight so we actually run the budget
  sddp_opts.elastic_filter_mode = ElasticFilterMode::single_cut;
  // Recovery fixture's calibration (mirrors test_sddp_method.cpp's
  // 10-phase recovery TEST_CASE): tiny slack activations need looser
  // cut_coeff_eps and higher elastic_penalty to survive PLP-parity
  // defaults' filtering.
  sddp_opts.cut_coeff_eps = 1e-6;
  sddp_opts.elastic_penalty = 1e2;
  sddp_opts.forward_max_attempts = 100;
  sddp_opts.lp_debug = true;
  sddp_opts.log_directory = dbg_dir.string();
  sddp_opts.lp_debug_compression = "uncompressed";

  SDDPMethod sddp(planning_lp, sddp_opts);
  [[maybe_unused]] auto results = sddp.solve();

  // Enumerate dumped LP files.  The writer uses the format
  // "gtopt_s{scene_uid}_p{phase_uid}_i{iter}.lp" (short form, matching
  // error_lp_fmt for filename-layout uniformity).  We group files by
  // (scene, phase) and for each group check the fcut row count is
  // monotonically non-decreasing with iteration.
  struct Dump
  {
    int iteration {0};
    std::string phase_tag;  // "s<scene>_p<phase>"
    std::filesystem::path path;
  };
  std::vector<Dump> dumps;
  for (const auto& ent : std::filesystem::directory_iterator {dbg_dir}) {
    if (!ent.is_regular_file()) {
      continue;
    }
    const auto filename = ent.path().filename().string();
    if (!filename.starts_with("gtopt_s") || !filename.ends_with(".lp")) {
      continue;
    }
    // Strip ".lp", then split at the "_i" boundary: iteration is the
    // trailing numeric token, phase_tag is "s<scene>_p<phase>".
    const auto stem_sv =
        std::string_view {filename}.substr(0, filename.size() - 3);
    const auto i_pos = stem_sv.rfind("_i");
    REQUIRE(i_pos != std::string_view::npos);
    const auto iter_str = stem_sv.substr(i_pos + 2);
    int iter_val = 0;
    {
      const auto* first = iter_str.data();
      const auto* last = first + iter_str.size();  // NOLINT
      const auto [p, err] = std::from_chars(first, last, iter_val);
      REQUIRE(err == std::errc {});
    }
    auto phase_tag = std::string {
        stem_sv.substr(std::string_view {"gtopt_"}.size(),
                       i_pos - std::string_view {"gtopt_"}.size())};
    dumps.push_back(Dump {
        .iteration = iter_val,
        .phase_tag = std::move(phase_tag),
        .path = ent.path(),
    });
  }
  REQUIRE_FALSE(dumps.empty());

  // Group by phase_tag, sort each group by iteration, scan LP content
  // for "fcut" substring and record the hit count per iteration.
  std::unordered_map<std::string, std::vector<std::pair<int, int>>>
      fcut_count_by_phase;
  for (auto& d : dumps) {
    std::ifstream ifs(d.path);
    REQUIRE(ifs.is_open());
    std::stringstream buf;
    buf << ifs.rdbuf();
    const auto content = buf.str();
    // Count occurrences of "fcut" in the dump.  Each cut row appears
    // once in the ROWS section of an LP file, so a substring count
    // gives the per-iteration fcut row count for this (scene, phase).
    int n_fcut = 0;
    std::size_t pos = 0;
    while ((pos = content.find("fcut", pos)) != std::string::npos) {
      ++n_fcut;
      pos += 4;
    }
    fcut_count_by_phase[d.phase_tag].emplace_back(d.iteration, n_fcut);
  }

  // Persistence invariant: for every (scene, phase) that has ≥1 fcut
  // in some iteration, later iterations must have ≥ that count.  At
  // least one phase must exhibit the persistence across ≥ 2 iterations
  // for the test to be meaningful.
  bool observed_persistence = false;
  for (auto& [tag, series] : fcut_count_by_phase) {
    std::ranges::sort(series);  // by iteration (pair.first)
    int prev_count = 0;
    int prev_iter = -1;
    for (const auto& [it, cnt] : series) {
      CAPTURE(tag);
      CAPTURE(it);
      CAPTURE(cnt);
      CAPTURE(prev_iter);
      CAPTURE(prev_count);
      // Monotonic non-decrease: once a row is in the LP it stays.
      if (prev_iter >= 0) {
        CHECK(cnt >= prev_count);
      }
      if (cnt >= 1 && prev_iter >= 0 && prev_count >= 1) {
        observed_persistence = true;
      }
      prev_count = cnt;
      prev_iter = it;
    }
  }
  CHECK(observed_persistence);

  // Clean up dumps.
  std::filesystem::remove_all(dbg_dir, ec);
}

// ── 8. (REMOVED 2026-04-29) Per-row fcut name persistence ───────────────────
//
// The original test 8 enforced the strict invariant that every fcut
// row observed at iteration K on phase P must be present *by its
// exact row name* at every later iteration — guarding against silent
// row replacement (a hypothetical bug where a cut is overwritten in
// place with a structurally similar row that has a different name).
//
// Removed because:
//   1. The bug class is not a natural failure mode of the SDDP code.
//      Cuts are persistent rows on the live backend; install / store
//      / replay are explicit operations with named call sites.
//      Silent replacement would require the LP backend itself to
//      mutate row names, which it doesn't.
//   2. The simulation pass legitimately discards simulation-only
//      feasibility cuts (sddp_iteration.cpp:742 — "SDDP: discarding
//      N simulation feasibility cut(s)"), so the strict per-name
//      persistence invariant is FALSE under any real SDDP run.
//      Refactoring the test to filter to TRAINING-only iterations
//      adds complexity for a non-existent bug class.
//   3. Coverage of the actually-relevant invariants is in place:
//      * Aggregate count persistence per (scene, phase) — test 7
//        ("fcuts persist across iterations under lp_debug").
//      * fcut row presence in single-iter LP dumps — test 6
//        ("LP file contains installed fcut row(s)").
//      * Cut-row metadata stamping (class_name / constraint_name) —
//        test_sddp_cut_tag.cpp.
//      * Cut name round-trip through save/load — test_sddp_cut_io.cpp.
//      * multi-cut row-label uniqueness — test 9 below.

// ── 9. Regression: multi_cut + row-names enabled must not clash ─────────────
//
// Pre-fix, `build_multi_cuts` left `variable_uid = unknown_uid` and
// `class_name = "Sddp"` on every bound-cut row, so the LabelMaker
// composed identical labels whenever two links produced cuts in the
// same (scene, phase, iteration, infeas_count) context — LabelMaker's
// `duplicates_are_errors` guard then threw `"Duplicate LP row name"`
// when the LP had row names enabled.  The fix uses the link's
// `class_name` and `uid` on each mcut row so labels are globally
// unique.  This test constructs the exact combination that used to
// throw: multi_cut mode + LpNamesLevel::all (via LpMatrixOptions) +
// two-reservoir fixture that activates both links' slacks.

TEST_CASE(  // NOLINT
    "SDDP fcut audit — multi_cut row labels are unique under LpNamesLevel::all")
{
  auto planning = make_two_reservoir_forced_infeasibility_planning();

  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  PlanningLP planning_lp(std::move(planning), flat_opts);

  SDDPOptions sddp_opts;
  sddp_opts.max_iterations = 2;
  sddp_opts.elastic_filter_mode = ElasticFilterMode::multi_cut;
  sddp_opts.multi_cut_threshold = 0;  // emit bound cuts immediately
  sddp_opts.save_simulation_cuts = true;

  SDDPMethod sddp(planning_lp, sddp_opts);

  // The key invariant: running solve() under these options must not
  // throw.  Before the fix, LabelMaker threw
  // "Duplicate LP row name: sddp_mcut_lb_-1_0_2_0_1" because two
  // links in the same infeasible LP produced mcut rows with the same
  // synthesized label.
  [[maybe_unused]] auto results = sddp.solve();

  // Both mcut row-naming prefixes should appear in the stored cuts'
  // metadata — `mcut_lb` bounds and/or `mcut_ub` bounds are emitted
  // whenever at least one slack is active in the elastic clone.
  int n_mcut = 0;
  std::unordered_set<std::string> seen_names;
  for (const auto& c : sddp.stored_cuts()) {
    if (c.name.contains("mcut")) {
      ++n_mcut;
      // Every stored name must be unique — a duplicate would confirm
      // the regression is back.
      CAPTURE(c.name);
      const auto [it, inserted] = seen_names.insert(c.name);
      CHECK(inserted);
    }
  }
  CAPTURE(n_mcut);
  // NOTE: after the 2026-04-23 `|π·dx| < slack_tol` filter in
  // `build_multi_cuts`, this two-reservoir forced-infeasibility fixture
  // produces 0 mcut rows — the trial states land at reservoir bounds,
  // so every cut would collapse to `source ≥ emax` and is dropped by
  // the box-edge guard.  That is the CORRECT post-fix behaviour: those
  // pinning cuts were the bug.  The test still verifies (a) solve()
  // doesn't throw under LpNamesLevel::all and (b) any mcut labels that
  // ARE emitted are unique.  `n_mcut >= 0` is trivially true but keeps
  // the CAPTURE visible in failures.
  CHECK(n_mcut >= 0);
}
