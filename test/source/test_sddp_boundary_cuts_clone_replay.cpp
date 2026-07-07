// SPDX-License-Identifier: BSD-3-Clause
//
// Clone-replay persistency for loaded boundary cuts.
//
// The aperture-pass at the terminal phase clones the source LP — via
// `clone(CloneKind::shallow)` under `LowMemoryMode::off` (native
// CPXcloneprob/HiGHS clone of the live backend) and via
// `clone_from_flat(CloneKind::shallow, with_replay=true)` under
// `compress` (manual `load_flat` + `apply_post_load_replay`).  Both
// routes must preserve the boundary-cut row(s) that constrain α_T
// from below — otherwise α_T enters the aperture clone's objective at
// coefficient +1 with no lower bound and the LP returns
// `CPX_STAT_UNBOUNDED` (logged as "infeasible (status 2)" by the
// aperture pass) for every aperture at the terminal phase.  That
// degeneracy is the precise pathology observed on
// `support/juan/gtopt_iplp_plain` iter 20: gap collapsed from 90.5%
// back to 99.6% because the cuts on α_T disappeared inside aperture
// clones at phase 51.
//
// This test pins both clone routes' behavior in a single parametric
// pass: under `off` and `compress` the boundary-cut row count and its
// (α-coefficient, state-variable coefficient, RHS) tuple must be
// preserved verbatim in the cloned LP.  Under `off` the cut is
// `record_cut_row`'ed (post-`6cf57176` the replay buffer tracks under
// every mode), but the clone path itself is the native backend clone,
// so the cut survives via the backend copy — the assertion still
// holds and serves as a regression guard if the install path ever
// gates again on `mode != off`.

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>
#include <unistd.h>

#include "sddp_helpers.hpp"

using namespace gtopt;
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace
{

[[nodiscard]] auto find_efin_col(const PlanningLP& planning_lp,
                                 SceneIndex scene_index,
                                 PhaseIndex phase_index,
                                 Uid uid) -> std::optional<ColIndex>
{
  const auto& svar_map =
      planning_lp.simulation().state_variables(scene_index, phase_index);
  for (const auto& [key, svar] : svar_map) {
    if (key.uid == uid && key.col_name == std::string_view {"efin"}) {
      return svar.col();
    }
  }
  return std::nullopt;
}

/// Capture (row_low, alpha_coeff, state_coeff) for the boundary cut row
/// just installed on `li`.  The row is at index `cut_row_idx` and is
/// expected to carry exactly two non-zero entries: `1.0` on α and the
/// negated physical coefficient on the state variable.
struct CutRowFingerprint
{
  double row_low;
  double alpha_coeff;
  double state_coeff;

  [[nodiscard]] static auto capture(const LinearInterface& li,
                                    RowIndex row_idx,
                                    ColIndex alpha_col,
                                    ColIndex state_col) -> CutRowFingerprint
  {
    return CutRowFingerprint {
        .row_low = li.get_row_low()[row_idx],
        .alpha_coeff = li.get_coeff(row_idx, alpha_col),
        .state_coeff = li.get_coeff(row_idx, state_col),
    };
  }
};

/// Full structural fingerprint of a LinearInterface used to compare a
/// clone against its source LP.  We capture only the readback fields
/// that should be value-equal across a successful clone — sizes,
/// dense row/col bound and cost arrays, the row at the boundary-cut
/// index, and the scaling vectors that drive `compose_physical`.
///
/// Note: `m_base_numrows_` is intentionally NOT compared here.  The
/// clone re-runs `save_base_numrows()` inside `apply_post_load_replay`
/// after re-applying the dynamic rows, so the clone's base_numrows
/// includes those rows while the source's was set at
/// `freeze_for_cuts` time (before any dynamic rows / cuts were
/// added).  That asymmetry is a known design choice — cut deletion
/// uses the LOCAL base_numrows to index `m_active_cuts_`, and as long
/// as the clone consistently uses its own base, deletions land on
/// the right rows.  See `linear_interface.cpp:619-625` for the
/// rationale.  The full row content equality check below is the
/// invariant we actually want — base_numrows just tags where the
/// dynamic block begins.
struct LpBoundsFingerprint
{
  Index numrows;
  Index numcols;
  std::vector<double> row_low;
  std::vector<double> row_upp;
  std::vector<double> col_low;
  std::vector<double> col_upp;
  std::vector<double> col_cost;
  std::vector<double> col_scales;
  std::vector<double> row_scales;
  double scale_objective;

  [[nodiscard]] static auto capture(const LinearInterface& li)
      -> LpBoundsFingerprint
  {
    const auto nrows = li.get_numrows();
    const auto ncols = li.get_numcols();
    LpBoundsFingerprint fp {
        .numrows = nrows,
        .numcols = ncols,
        .row_low = {},
        .row_upp = {},
        .col_low = {},
        .col_upp = {},
        .col_cost = {},
        .col_scales = {},
        .row_scales = {},
        .scale_objective = 0.0,
    };
    fp.row_low.reserve(static_cast<std::size_t>(nrows));
    fp.row_upp.reserve(static_cast<std::size_t>(nrows));
    fp.col_low.reserve(static_cast<std::size_t>(ncols));
    fp.col_upp.reserve(static_cast<std::size_t>(ncols));
    fp.col_cost.reserve(static_cast<std::size_t>(ncols));

    const auto rlo = li.get_row_low();
    const auto rup = li.get_row_upp();
    for (Index r = 0; r < nrows; ++r) {
      fp.row_low.push_back(rlo[RowIndex {r}]);
      fp.row_upp.push_back(rup[RowIndex {r}]);
    }
    const auto clo = li.get_col_low();
    const auto cup = li.get_col_upp();
    const auto cost = li.get_col_cost();
    for (Index c = 0; c < ncols; ++c) {
      fp.col_low.push_back(clo[ColIndex {c}]);
      fp.col_upp.push_back(cup[ColIndex {c}]);
      fp.col_cost.push_back(cost[ColIndex {c}]);
    }

    // Scale vectors are exposed as `const auto&` references to the
    // internal storage — copy element-by-element so the fingerprint
    // owns its data even after the source LI is destroyed.
    const auto& cs = li.get_col_scales();
    fp.col_scales.assign(cs.begin(), cs.end());
    const auto& rs = li.get_row_scales();
    fp.row_scales.assign(rs.begin(), rs.end());

    return fp;
  }
};

/// Pairwise CHECK on every component of two `LpBoundsFingerprint`s.
/// Element-wise tolerance uses `default_sddp_cut_coeff_eps` so the
/// comparison matches the precision the cut install / replay path
/// works with — exact equality would be too strict because
/// `compose_physical` runs FP multiplications during add_row.
///
/// **Infinity handling**: ``doctest::Approx`` does NOT compare
/// infinities (``inf == Approx(inf)`` is FALSE under doctest's
/// tolerance check because the relative-tolerance formula computes
/// ``|inf - inf| = NaN`` and fails the comparison).  Unbounded LP
/// rows/cols legitimately carry ``+/-inf``, and OSI/CLP exposes them
/// as exact ``inf`` (vs ``DblMax`` on some other backends).  Use a
/// helper that short-circuits to exact equality when either operand
/// is infinite.
inline void check_lp_fingerprints_equal(const LpBoundsFingerprint& a,
                                        const LpBoundsFingerprint& b)
{
  const auto eps = PlanningOptionsLP::default_sddp_cut_coeff_eps;
  REQUIRE(a.numrows == b.numrows);
  REQUIRE(a.numcols == b.numcols);
  REQUIRE(a.row_low.size() == b.row_low.size());
  REQUIRE(a.row_upp.size() == b.row_upp.size());
  REQUIRE(a.col_low.size() == b.col_low.size());
  REQUIRE(a.col_upp.size() == b.col_upp.size());
  REQUIRE(a.col_cost.size() == b.col_cost.size());
  REQUIRE(a.col_scales.size() == b.col_scales.size());
  REQUIRE(a.row_scales.size() == b.row_scales.size());

  // Inf-tolerant approximate equality.  Returns true when both
  // operands are equal (covering inf == inf, -inf == -inf, NaN
  // pairs treated as equal here so unset bounds don't trip the
  // comparison) OR when the doctest::Approx relative-tolerance
  // check passes.  Lambda-local to keep the helper self-contained.
  const auto approx_eq = [eps](double x, double y) -> bool
  {
    if (std::isinf(x) || std::isinf(y) || std::isnan(x) || std::isnan(y)) {
      return std::isinf(x) == std::isinf(y) && (std::isnan(x) == std::isnan(y))
          && (!std::isfinite(x) ? x == y : true);
    }
    return x == doctest::Approx(y).epsilon(eps);
  };

  for (std::size_t i = 0; i < a.row_low.size(); ++i) {
    CHECK(approx_eq(a.row_low[i], b.row_low[i]));
    CHECK(approx_eq(a.row_upp[i], b.row_upp[i]));
  }
  for (std::size_t i = 0; i < a.col_low.size(); ++i) {
    CHECK(approx_eq(a.col_low[i], b.col_low[i]));
    CHECK(approx_eq(a.col_upp[i], b.col_upp[i]));
    CHECK(approx_eq(a.col_cost[i], b.col_cost[i]));
  }
  for (std::size_t i = 0; i < a.col_scales.size(); ++i) {
    CHECK(approx_eq(a.col_scales[i], b.col_scales[i]));
  }
  for (std::size_t i = 0; i < a.row_scales.size(); ++i) {
    CHECK(approx_eq(a.row_scales[i], b.row_scales[i]));
  }
}

/// Cross-check the full sparse content of one row by reading every
/// coefficient `get_coeff(row, c)` for c in [0, numcols).  Two clones
/// of the same LP must produce identical coefficients at every cell
/// of the row — not just the (α, state) pair already checked by
/// `CutRowFingerprint`.
inline void check_row_coeffs_equal(const LinearInterface& a,
                                   const LinearInterface& b,
                                   RowIndex row_idx)
{
  REQUIRE(a.get_numcols() == b.get_numcols());
  const auto eps = PlanningOptionsLP::default_sddp_cut_coeff_eps;
  for (Index c = 0; c < a.get_numcols(); ++c) {
    const auto col = ColIndex {c};
    const auto a_coef = a.get_coeff(row_idx, col);
    const auto b_coef = b.get_coeff(row_idx, col);
    CHECK(a_coef == doctest::Approx(b_coef).epsilon(eps));
  }
}

[[nodiscard]] auto write_one_cut_csv(double rhs, double state_coeff)
    -> std::string
{
  // Unique per (process, call) so the multiple TEST_CASEs in this file —
  // each a separate `ctest -j` process running the same binary — never
  // race on a shared fixed path.
  static std::atomic<unsigned> counter {0};
  const auto tmp_file = (std::filesystem::temp_directory_path()
                         / std::format("gtopt_test_bdr_clone_replay_{}_{}.csv",
                                       ::getpid(),
                                       counter.fetch_add(1)))
                            .string();
  std::ofstream ofs(tmp_file);
  ofs << "iteration,scene,rhs,rsv1\n";
  ofs << "1,0," << rhs << "," << state_coeff << "\n";
  return tmp_file;
}

}  // namespace

TEST_CASE(  // NOLINT
    "boundary cuts survive clone — off mode (native clone backend copy)")
{
  // Build a 3-phase hydro fixture under `LowMemoryMode::off`.  The
  // live backend stays alive for the entire test, and
  // `clone(CloneKind::shallow)` goes through the backend's native
  // clone() (which copies the live LP including the boundary cut).
  // `clone_from_flat` is NOT available in off mode (it asserts on
  // `has_snapshot_data()`), so the test exercises the native path
  // exclusively.
  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::off;
  PlanningLP planning_lp(std::move(planning));

  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;
  const auto tmp_file = write_one_cut_csv(phys_rhs, phys_coeff);

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);
  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto last_phase = planning_lp.simulation().last_phase_index();
  auto& source_sys = planning_lp.system(first_scene_index(), last_phase);
  auto& source_li = source_sys.linear_interface();

  const auto pre_numrows = source_li.get_numrows();
  const auto state_col =
      find_efin_col(planning_lp, first_scene_index(), last_phase, Uid {1});
  REQUIRE(state_col.has_value());

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);
  REQUIRE(source_li.get_numrows() == pre_numrows + 1);

  const auto cut_row = RowIndex {static_cast<Index>(pre_numrows)};

  // Resolve α col index on the SOURCE LP — same index will be used
  // by the cloned LP because `add_cols(dynamic_cols)` replays α at
  // the same slot, AND under native clone() the LP is a verbatim copy.
  const auto* alpha_svar = find_alpha_state_var(
      planning_lp.simulation(), first_scene_index(), last_phase);
  REQUIRE(alpha_svar != nullptr);
  const auto alpha_col = alpha_svar->col();

  const auto source_fp =
      CutRowFingerprint::capture(source_li, cut_row, alpha_col, *state_col);
  CAPTURE(source_fp.row_low);
  CAPTURE(source_fp.alpha_coeff);
  CAPTURE(source_fp.state_coeff);

  // Capture the FULL source LP fingerprint (sizes, all bounds, all
  // costs, scale vectors) for whole-LP equivalence checks below.
  const auto source_lp_fp = LpBoundsFingerprint::capture(source_li);

  // ── Native shallow clone — should carry the cut row verbatim. ──
  auto clone = source_li.clone(LinearInterface::CloneKind::shallow);

  // 1) Size + dense readback equivalence (numrows, numcols, every
  //    row/col bound, every objective coefficient, scale vectors).
  const auto clone_lp_fp = LpBoundsFingerprint::capture(clone);
  check_lp_fingerprints_equal(source_lp_fp, clone_lp_fp);

  // 2) Boundary-cut row content: the targeted (α, state) coefficients
  //    plus the row's lower bound.
  const auto clone_cut_fp =
      CutRowFingerprint::capture(clone, cut_row, alpha_col, *state_col);
  CHECK(clone_cut_fp.row_low
        == doctest::Approx(source_fp.row_low)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  CHECK(clone_cut_fp.alpha_coeff
        == doctest::Approx(source_fp.alpha_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  CHECK(clone_cut_fp.state_coeff
        == doctest::Approx(source_fp.state_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));

  // 3) Full sparse content of the boundary-cut row — every column
  //    coefficient (including the implicit zeros) must match.
  //    Catches any cell that drifts under a future cut-precision
  //    refactor where the (α, state) pair stays correct but a
  //    spurious entry appears elsewhere.
  check_row_coeffs_equal(source_li, clone, cut_row);

  // 4) Solver-name preservation: the clone should run on the same
  //    backend the source was attached to, otherwise downstream
  //    aperture solves silently pick a different LP backend.
  CHECK(clone.solver_name() == source_li.solver_name());

  std::filesystem::remove(tmp_file);
}

TEST_CASE(  // NOLINT
    "boundary cuts survive clone — compress mode "
    "(clone_from_flat with_replay=true)")
{
  // Compress mode is the SDDP / cascade production default (commit
  // `71d0da7d`).  The aperture pass under `use_manual_clone=true`
  // takes the `clone_from_flat(with_replay=true)` route: load the
  // pre-cut snapshot via `load_flat`, then `apply_post_load_replay`
  // re-adds the dynamic cols (α), the dynamic rows, and the
  // boundary cuts from `m_replay_.active_cuts()`.  This test pins
  // the boundary-cut survival across that round-trip.
  //
  // Before this regression guard, the gtopt_iplp_plain juan run
  // observed *every* aperture at the terminal phase coming back as
  // `CPX_STAT_UNBOUNDED` at iter 20+ because α_T was unconstrained
  // in the clone — the boundary cuts dropped out of the replay
  // path.  With `add_cut_row` as the unified boundary-cut installer
  // (post the 2026-05-11 refactor of `sddp_boundary_cuts.cpp`)
  // every cut goes through `record_cut_row` and survives both the
  // release / reconstruct cycle and the clone_from_flat cycle.
  auto planning = make_3phase_hydro_planning();
  planning.options.method = MethodType::sddp;
  planning.options.sddp_options.low_memory_mode = LowMemoryMode::compress;
  PlanningLP planning_lp(std::move(planning));

  constexpr double phys_rhs = 42.0;
  constexpr double phys_coeff = 5.0;
  const auto tmp_file = write_one_cut_csv(phys_rhs, phys_coeff);

  SDDPOptions opts;
  opts.boundary_cuts_mode = BoundaryCutsMode::separated;
  opts.boundary_max_iterations = 0;

  const LabelMaker label_maker {LpNamesLevel::none};
  auto states = make_default_scene_phase_states(planning_lp);
  register_alpha_variables(planning_lp, first_scene_index(), 1.0);

  const auto last_phase = planning_lp.simulation().last_phase_index();
  auto& source_sys = planning_lp.system(first_scene_index(), last_phase);
  auto& source_li = source_sys.linear_interface();

  // The compress fixture must have a snapshot for clone_from_flat
  // to be available; the snapshot lands during `freeze_for_cuts`.
  REQUIRE(source_li.has_snapshot_data());

  const auto pre_numrows = source_li.get_numrows();
  const auto state_col =
      find_efin_col(planning_lp, first_scene_index(), last_phase, Uid {1});
  REQUIRE(state_col.has_value());

  const auto result =
      load_boundary_cuts_csv(planning_lp, tmp_file, opts, label_maker, states);
  REQUIRE(result.has_value());
  REQUIRE(result->count == 1);
  REQUIRE(source_li.get_numrows() == pre_numrows + 1);

  const auto cut_row = RowIndex {static_cast<Index>(pre_numrows)};

  const auto* alpha_svar = find_alpha_state_var(
      planning_lp.simulation(), first_scene_index(), last_phase);
  REQUIRE(alpha_svar != nullptr);
  const auto alpha_col = alpha_svar->col();

  const auto source_fp =
      CutRowFingerprint::capture(source_li, cut_row, alpha_col, *state_col);
  CAPTURE(source_fp.row_low);
  CAPTURE(source_fp.alpha_coeff);
  CAPTURE(source_fp.state_coeff);

  // Capture the FULL source LP fingerprint BEFORE the
  // DecompressionGuard scope opens so the comparison runs against the
  // pre-guard state (the guard only toggles a compression flag on the
  // snapshot — it does not mutate the live backend the source reads
  // from).
  const auto source_lp_fp = LpBoundsFingerprint::capture(source_li);

  // ── Manual clone via clone_from_flat — must replay the boundary cut. ──
  // The compress aperture path takes this route when
  // `use_manual_clone=true` (parallel-safe, no global solver mutex).
  // Wrapped in `DecompressionGuard` to mirror the production path
  // (`sddp_aperture_pass.cpp:460 / 693`) — the snapshot's flat LP is
  // kept compressed between aperture windows; the guard decompresses
  // it on construction and re-compresses on scope exit.
  const DecompressionGuard guard(source_li);
  auto clone = source_li.clone_from_flat(LinearInterface::CloneKind::shallow);

  // 1) Size + dense readback equivalence — under compress mode this
  //    is the load-bearing invariant: `load_flat` restored the pre-
  //    cut snapshot then `apply_post_load_replay` re-added the
  //    dynamic cols (α), dynamic rows, AND active cuts.  All three
  //    must land at the right indices with the right bounds / costs.
  const auto clone_lp_fp = LpBoundsFingerprint::capture(clone);
  check_lp_fingerprints_equal(source_lp_fp, clone_lp_fp);

  // 2) Boundary-cut row content: the targeted (α, state) pair and
  //    the row's lower bound (= cut RHS) must match.  If
  //    `m_active_cuts_` lost the boundary cut, `clone.get_numrows()`
  //    would be `pre_numrows` (the size-check above already failed),
  //    so this fingerprint guards the second-order case: the cut row
  //    is present but `compose_physical` double-applied a scale
  //    factor.
  const auto clone_cut_fp =
      CutRowFingerprint::capture(clone, cut_row, alpha_col, *state_col);
  CHECK(clone_cut_fp.row_low
        == doctest::Approx(source_fp.row_low)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  CHECK(clone_cut_fp.alpha_coeff
        == doctest::Approx(source_fp.alpha_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));
  CHECK(clone_cut_fp.state_coeff
        == doctest::Approx(source_fp.state_coeff)
               .epsilon(PlanningOptionsLP::default_sddp_cut_coeff_eps));

  // 3) Full sparse content of the boundary-cut row.  Same rationale
  //    as the off-mode test — under compress this is even stricter
  //    because the row goes through `compose_physical` again during
  //    `add_rows(active_cuts)` in `apply_post_load_replay`.
  check_row_coeffs_equal(source_li, clone, cut_row);

  // 4) Solver-name preservation — `clone_from_flat` constructs the
  //    clone from `m_solver_name_` and must not silently switch
  //    backends mid-cycle.
  CHECK(clone.solver_name() == source_li.solver_name());

  std::filesystem::remove(tmp_file);
}

// NOLINTEND(bugprone-unchecked-optional-access)
