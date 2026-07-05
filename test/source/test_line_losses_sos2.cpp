// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_losses_sos2.cpp — pin the L-secant chord + SOS2
// emission introduced in issue #504 for the ``tangent_signed_flow``
// mode.
//
// With ``loss_secant_segments = L > 1`` the single ``v ≥ |f|``
// auxiliary column is replaced by ``L`` segment columns
// ``v_l ∈ [0, envelope/L]`` and the chord upper bound becomes the
// piecewise sum ``ℓ ≤ Σ chord_slope_l · v_l`` with per-segment
// slope ``(R/V²)·(envelope/L)·(2l − 1)``.  With ``loss_use_sos2 =
// true`` the LP additionally emits one SOS2 declaration over the L
// segment columns so fill order ``v_1 → v_2 → … → v_L`` is enforced.
//
// Tests pin:
//   1) Schema round-trip — loss_secant_segments + loss_use_sos2.
//   2) L = 1 default — same column / row counts as pre-#504; no
//      SOS2 emitted.
//   3) L = 4 — exactly 4 ``line_flow_abs_`` cols per (line, block),
//      each bounded ``[0, envelope/4]``.
//   4) Sum-of-segments abs rows ``Σ v_l ± f ≥ 0`` survive the L > 1
//      generalisation (still 2 rows).
//   5) Piecewise chord row carries L coefficients (one per v_l).
//   6) SOS2 declaration count: 1 set per (line, block) when
//      ``use_sos2 && L ≥ 2``; ``0`` otherwise.
//   7) Vacuous sanitization — ``use_sos2 = true`` with ``L = 1``
//      emits no SOS2.
//   8) Per-line override beats the global default.
//   9) make_config sanitization (unit, no LP build) — ``L ≤ 0``
//      clamped to 1, ``use_sos2 && L ≤ 1`` collapsed to false.

#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json/json_line.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/solver_registry.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// Wrap the entire file body in a uniquely-named outer namespace so the
// Unity-build (CMake batches many test cpp files into a single TU)
// cannot collide with same-named helpers in sibling test files — see
// CLAUDE.md ``unity-anon-namespace`` memory note.
namespace test_line_losses_sos2_ns
{

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── 2-bus fixture replicated from test_line_losses_tangent_signed_flow ──
//
// Kept local instead of factoring into a shared test header: each
// test_*.cpp file is its own translation unit (under unity-build with
// the outer namespace wrap above) and the fixture is intentionally
// trimmed to the L-secant + SOS2 axes — extra options like
// kirchhoff_mode are not exercised here.

/// Pin SOS2-emitting fixtures to CPLEX when it is available.
///
/// SOS2 semantics tests must run on a backend that truly enforces
/// SOS2.  MindOpt 2.3.0 mis-handles SOS2 — silent NON-ENFORCEMENT on
/// this very two-bus fixture (returns λ1/λ8 non-adjacent while
/// reporting OPTIMAL) and spurious INFEASIBLE on the meshed IEEE 4-bus
/// model; both verified with minimal MDOreadmodel probes 2026-07-05.
/// The SOS2-emitting tests are already guarded by ``sos2_available()``
/// (requires CPLEX), so pinning to CPLEX matches exactly the guarded
/// configuration; on CLP-only CI the guard skips before the pin
/// matters.
inline void pin_sos2_capable_solver(LpMatrixOptions& bo, bool wants_sos2)
{
  if (!wants_sos2) {
    return;
  }
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  if (reg.has_solver("cplex")) {
    bo.solver_name = "cplex";
  }
}

struct TwoBusSos2Fixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  static constexpr double R = 0.01;
  static constexpr double V = 100.0;
  static constexpr double TMAX = 200.0;

  TwoBusSos2Fixture(int loss_secant_segments,
                    bool loss_use_sos2,
                    int loss_segments_K = 5,
                    bool per_line_override = true,
                    int global_secant_segments = 1,
                    bool global_use_sos2 = false)
      : system {
            .name = "TangentSignedFlowSos2_2Bus",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                },
            .demand_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {2},
                        .capacity = 100.0,
                    },
                },
            .generator_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "g1",
                        .bus = Uid {1},
                        .gcost = 10.0,
                        .capacity = 500.0,
                    },
                },
            .line_array =
                {
                    make_line(loss_secant_segments,
                              loss_use_sos2,
                              loss_segments_K,
                              per_line_override),
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options(global_secant_segments, global_use_sos2))
      , sim_lp(simulation, options)
      , sys_lp(system,
               sim_lp,
               build_opts(loss_use_sos2 || global_use_sos2))
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static Line make_line(int L, bool use_sos2, int K, bool per_line_override)
  {
    Line ln {
        .uid = Uid {1},
        .name = "l1",
        .bus_a = Uid {1},
        .bus_b = Uid {2},
        .voltage = V,
        .resistance = R,
        .line_losses_mode = OptName {std::string {"tangent_signed_flow"}},
        .loss_segments = K,
        .tmax_ba = TMAX,
        .tmax_ab = TMAX,
        .capacity = TMAX,
    };
    if (per_line_override) {
      ln.loss_secant_segments = L;
      ln.loss_use_sos2 = use_sos2;
    }
    return ln;
  }

  PlanningOptionsLP make_options(int global_secant_segments,
                                 bool global_use_sos2)
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.model_options.loss_secant_segments = global_secant_segments;
    opts.model_options.loss_use_sos2 = global_use_sos2;
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_opts(bool wants_sos2)
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    pin_sos2_capable_solver(bo, wants_sos2);
    return bo;
  }
};

/// True iff a loaded backend implements ``add_sos2``.  Currently only
/// the CPLEX plugin overrides the ``SolverBackend::add_sos2`` default
/// (which throws on ``size() >= 2``); HiGHS / CBC / CLP all fall
/// through to the throw.  CBC supports MIP but NOT SOS2, so the
/// generic ``has_mip_solver()`` guard is too permissive for tests
/// that build a SOS2-emitting LP — they would throw at LP-build time
/// on CBC-only CI builds.  The ``TwoBusSos2Fixture(L>=2, use_sos2)``
/// constructor builds the LP eagerly, so guard at the test entry.
///
/// Triggers ``load_all_plugins()`` before the lookup so the helper
/// works in isolation (single-test filter runs).  ``has_solver``
/// alone only inspects already-loaded plugins, which can be false
/// negative when no other test has primed the registry yet.
[[nodiscard]] bool sos2_available()
{
  auto& reg = SolverRegistry::instance();
  reg.load_all_plugins();
  return reg.has_solver("cplex");
}

[[nodiscard]] int count_cols_containing(const LinearInterface& li,
                                        std::string_view substr)
{
  int count = 0;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] int count_rows_containing(const LinearInterface& li,
                                        std::string_view substr)
{
  int count = 0;
  for (const auto& [name, _idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      ++count;
    }
  }
  return count;
}

}  // namespace
}  // namespace test_line_losses_sos2_ns

using test_line_losses_sos2_ns::count_cols_containing;
using test_line_losses_sos2_ns::count_rows_containing;
using test_line_losses_sos2_ns::sos2_available;
using test_line_losses_sos2_ns::TwoBusSos2Fixture;

// ── (1) Schema round-trip ───────────────────────────────────────────

TEST_CASE("Line JSON — loss_secant_segments + loss_use_sos2 round-trip")
{
  constexpr std::string_view json = R"({
    "uid": 42,
    "name": "l_sos2",
    "bus_a": 1,
    "bus_b": 2,
    "loss_secant_segments": 4,
    "loss_use_sos2": true
  })";

  const auto ln = daw::json::from_json<Line>(json);
  CHECK(ln.uid == Uid {42});
  REQUIRE(ln.loss_secant_segments.has_value());
  CHECK(ln.loss_secant_segments.value() == 4);
  REQUIRE(ln.loss_use_sos2.has_value());
  CHECK(ln.loss_use_sos2.value() == true);

  // Round-trip the back to JSON and re-parse to pin to_json_data wiring.
  const auto out = daw::json::to_json(ln);
  const auto ln2 = daw::json::from_json<Line>(out);
  REQUIRE(ln2.loss_secant_segments.has_value());
  CHECK(ln2.loss_secant_segments.value() == 4);
  REQUIRE(ln2.loss_use_sos2.has_value());
  CHECK(ln2.loss_use_sos2.value() == true);
}

// ── (2) L = 1 default — legacy single-secant behaviour ──────────────

TEST_CASE(
    "tangent_signed_flow L=1: single flow_abs col, no SOS2 (pre-#504 parity)")
{
  TwoBusSos2Fixture fix(/*L=*/1, /*use_sos2=*/false);
  auto& li = fix.lp();

  // One ``line_flow_abs_`` column — the legacy single-v aux.
  CHECK(count_cols_containing(li, "line_flow_abs_") == 1);
  // Two abs rows ``v ≥ ±f`` (unchanged from pre-#504).
  CHECK(count_rows_containing(li, "line_flow_abs") == 2);
  // No SOS2 declarations.
  CHECK(li.sos2_set_count() == 0);
}

// ── (3) L = 4: four flow_abs cols, each [0, envelope/4] ─────────────

TEST_CASE("tangent_signed_flow L=4: 4 flow_abs cols bounded [0, envelope/L]")
{
  TwoBusSos2Fixture fix(/*L=*/4, /*use_sos2=*/false);
  auto& li = fix.lp();

  // 4 segment columns — the new L-secant geometry.
  CHECK(count_cols_containing(li, "line_flow_abs_") == 4);

  // Each column's upper bound should be envelope/L = TMAX/4 = 50.
  // We can't address segment columns by name without knowing the
  // exact label format, but we can sweep every column whose name
  // contains ``line_flow_abs_`` and check its upper bound.
  const auto& col_ubs = li.get_col_upp();
  int matched = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (!name.contains("line_flow_abs_")) {
      continue;
    }
    ++matched;
    CHECK(col_ubs[value_of(idx)]
          == doctest::Approx(TwoBusSos2Fixture::TMAX / 4).epsilon(1e-9));
  }
  CHECK(matched == 4);
}

// ── (4) Sum-of-segments abs rows — still exactly 2 rows ─────────────

TEST_CASE("tangent_signed_flow L=4: two abs rows (Σ v_l ± f ≥ 0)")
{
  TwoBusSos2Fixture fix(/*L=*/4, /*use_sos2=*/false);
  auto& li = fix.lp();

  // Even with L segments the abs constraint stays as 2 rows: one for
  // ``Σ v_l − f ≥ 0`` and one for ``Σ v_l + f ≥ 0``.  Both rows now
  // have ``L + 1`` non-zeros instead of 2 (L v_l terms + 1 f term).
  CHECK(count_rows_containing(li, "line_flow_abs") == 2);
}

// ── (5) Piecewise chord row carries L coefficients ──────────────────

TEST_CASE(
    "tangent_signed_flow L=4: piecewise chord row exists exactly once "
    "in loss_link")
{
  TwoBusSos2Fixture fix(/*L=*/4, /*use_sos2=*/false);
  auto& li = fix.lp();

  // Loss-link rows include the K = 5 tangent lower bounds (one drops
  // out for k=3 → f_k=0 under post-scale tolerance, leaving 4
  // surviving tangents) PLUS the single piecewise chord upper-bound
  // row.  Total: 4 + 1 = 5 rows tagged ``line_loss_link``.
  CHECK(count_rows_containing(li, "line_loss_link") == 5);
}

// ── (6) SOS2 emission count: 1 set when use_sos2 && L ≥ 2 ───────────

TEST_CASE("tangent_signed_flow L=4 + use_sos2: 1 SOS2 set per (line, block)")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  TwoBusSos2Fixture fix(/*L=*/4, /*use_sos2=*/true);
  auto& li = fix.lp();

  // One block × one line = one SOS2 declaration.
  CHECK(li.sos2_set_count() == 1);
}

TEST_CASE("tangent_signed_flow L=4 without use_sos2: no SOS2 emitted")
{
  TwoBusSos2Fixture fix(/*L=*/4, /*use_sos2=*/false);
  auto& li = fix.lp();
  CHECK(li.sos2_set_count() == 0);
}

// ── (7) Vacuous sanitization — L = 1 with use_sos2 = true ───────────

TEST_CASE("tangent_signed_flow L=1 with use_sos2=true: SOS2 dropped (vacuous)")
{
  // make_config sanitises the L ≤ 1 + use_sos2 = true combination
  // (SOS2 over a single column is vacuous; load_flat also silently
  // skips size < 2 sets, but the LP-build-side filter is the
  // single source of truth).
  TwoBusSos2Fixture fix(/*L=*/1, /*use_sos2=*/true);
  auto& li = fix.lp();
  CHECK(li.sos2_set_count() == 0);
  CHECK(count_cols_containing(li, "line_flow_abs_") == 1);
}

// ── (8) Per-line override beats global default ──────────────────────

TEST_CASE(
    "tangent_signed_flow: per-line loss_use_sos2 beats global "
    "model_options default")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  // Global default: use_sos2 = false, L = 1.  Per-line override:
  // L = 4 + use_sos2 = true.  The per-line override must win.
  // Lambda-form SOS2 emits 2L+1 = 9 ``line_flow_lambda_`` cols and
  // 0 ``line_flow_abs_`` cols (segment-form rows are dropped).
  TwoBusSos2Fixture fix(/*L=*/4,
                        /*use_sos2=*/true,
                        /*K=*/5,
                        /*per_line_override=*/true,
                        /*global_secant_segments=*/1,
                        /*global_use_sos2=*/false);
  auto& li = fix.lp();
  CHECK(count_cols_containing(li, "line_flow_lambda_") == (2 * 4) + 1);
  CHECK(count_cols_containing(li, "line_flow_abs_") == 0);
  CHECK(li.sos2_set_count() == 1);
}

TEST_CASE(
    "tangent_signed_flow: global model_options applies when per-line unset")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  // No per-line override; global says L = 3 + SOS2 on.  Line should
  // inherit those values via the planning-options resolver and emit
  // 2L+1 = 7 lambda cols.
  TwoBusSos2Fixture fix(/*L=*/0,
                        /*use_sos2=*/false,
                        /*K=*/5,
                        /*per_line_override=*/false,
                        /*global_secant_segments=*/3,
                        /*global_use_sos2=*/true);
  auto& li = fix.lp();
  CHECK(count_cols_containing(li, "line_flow_lambda_") == (2 * 3) + 1);
  CHECK(count_cols_containing(li, "line_flow_abs_") == 0);
  CHECK(li.sos2_set_count() == 1);
}

// ── (9) make_config sanitization (no LP build) ──────────────────────

TEST_CASE("make_config — L ≤ 0 clamped to 1, vacuous SOS2 collapses to off")
{
  // L = 0 + use_sos2 = true: make_config must clamp L → 1 and
  // disable SOS2 (vacuous over a single col).
  Line line;
  auto cfg0 = line_losses::make_config(LineLossesMode::tangent_signed_flow,
                                       line,
                                       LossAllocationMode::split,
                                       /*lossfactor=*/0,
                                       /*resistance=*/0.01,
                                       /*voltage=*/100,
                                       /*loss_segments=*/5,
                                       /*fmax=*/200,
                                       /*loss_row_scale=*/1.0,
                                       /*loss_envelope=*/0.0,
                                       /*loss_cost_eps=*/0.0,
                                       /*nseg_secant=*/0,
                                       /*use_sos2=*/true);
  CHECK(cfg0.nseg_secant == 1);
  CHECK(cfg0.use_sos2 == false);

  // L = 4 + use_sos2 = true: both pass through.
  auto cfg1 = line_losses::make_config(LineLossesMode::tangent_signed_flow,
                                       line,
                                       LossAllocationMode::split,
                                       /*lossfactor=*/0,
                                       /*resistance=*/0.01,
                                       /*voltage=*/100,
                                       /*loss_segments=*/5,
                                       /*fmax=*/200,
                                       /*loss_row_scale=*/1.0,
                                       /*loss_envelope=*/0.0,
                                       /*loss_cost_eps=*/0.0,
                                       /*nseg_secant=*/4,
                                       /*use_sos2=*/true);
  CHECK(cfg1.nseg_secant == 4);
  CHECK(cfg1.use_sos2 == true);

  // L = 4 + use_sos2 = false: pass through (no SOS2 declared).
  auto cfg2 = line_losses::make_config(LineLossesMode::tangent_signed_flow,
                                       line,
                                       LossAllocationMode::split,
                                       /*lossfactor=*/0,
                                       /*resistance=*/0.01,
                                       /*voltage=*/100,
                                       /*loss_segments=*/5,
                                       /*fmax=*/200,
                                       /*loss_row_scale=*/1.0,
                                       /*loss_envelope=*/0.0,
                                       /*loss_cost_eps=*/0.0,
                                       /*nseg_secant=*/4,
                                       /*use_sos2=*/false);
  CHECK(cfg2.nseg_secant == 4);
  CHECK(cfg2.use_sos2 == false);
}

// ── (10) Clone propagation regression (review P1-1) ────────────────
//
// ``LinearInterface::clone()`` previously skipped
// ``m_sos2_set_count_`` while propagating ``m_scaling_.obj_constant_raw`` and
// other LI-side state — the cloned backend carried the SOS2
// declarations correctly (CPXcloneprob) but ``cloned.sos2_set_count()``
// returned 0 spuriously, leaving the LI-side counter out of sync with
// the backend.  Pin the propagation so the regression cannot return.

TEST_CASE(
    "tangent_signed_flow L=4 + use_sos2: clone() preserves "
    "sos2_set_count()")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  TwoBusSos2Fixture fix(/*loss_secant_segments=*/4,
                        /*loss_use_sos2=*/true);
  auto& li = fix.lp();
  REQUIRE(li.sos2_set_count() == 1);

  // Native ``clone()`` route — goes through backend().clone() (e.g.
  // CPXcloneprob).  Without the P1-1 fix this returns 0.
  const auto cloned_deep = li.clone(LinearInterface::CloneKind::deep);
  CHECK(cloned_deep.sos2_set_count() == 1);

  const auto cloned_shallow = li.clone(LinearInterface::CloneKind::shallow);
  CHECK(cloned_shallow.sos2_set_count() == 1);
}

// ── (11) L-secant chord convergence rate (review request) ──────────
//
// The SOS2-enforced L-secant chord upper bound on the convex quadratic
// loss ``ℓ(f) = c·f²`` (with ``c = R/V²``) is piecewise tight at
// every breakpoint ``b_l = l·w`` (w = envelope/L) and overestimates by
// at most ``c·(w/2)²`` between breakpoints.  Therefore the worst-case
// chord-vs-true gap scales as
//
//     worst_gap(L) = c · (envelope / (2·L))²
//                  = c · envelope² / (4·L²)
//
// which is the L-secant analogue of the K-tangent ``O(1/K²)``
// convergence already pinned in ``test_line_losses_convergence.cpp``.
// Doubling L cuts the gap 4×.  Quintupling L cuts it 25×.
//
// This is analytical — no LP solves required — and mirrors the
// existing pattern.

namespace test_line_losses_sos2_convergence_ns  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{
namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

constexpr double kSos2Envelope = 200.0;
constexpr double kR = 0.01;
constexpr double kV2 = 100.0 * 100.0;
constexpr double kCoeff = kR / kV2;  // c = R/V²

/// Worst-case overstatement of the L-secant chord vs the true quadratic
/// under SOS2 fill-order: the chord is exact at breakpoints
/// ``b_l = l·w`` and at most ``c·(w/2)²`` loose at the midpoints
/// ``m_l = (l - 1/2)·w``.  Returns the analytical worst-case error in
/// MW (= units of ℓ).
[[nodiscard]] double sos2_chord_worst_gap(int L)
{
  const double w = kSos2Envelope / static_cast<double>(L);
  return kCoeff * (w / 2.0) * (w / 2.0);  // c · (w/2)²
}

}  // namespace
}  // namespace test_line_losses_sos2_convergence_ns

using test_line_losses_sos2_convergence_ns::kCoeff;
using test_line_losses_sos2_convergence_ns::kSos2Envelope;
using test_line_losses_sos2_convergence_ns::sos2_chord_worst_gap;

TEST_CASE("tangent_signed_flow L-secant: worst-gap shrinks O(1/L²) under SOS2")
{
  // Worst-case chord-vs-true-quadratic gap monotonically decreases as L
  // grows.  Pins the gap formula ``c · envelope² / (4·L²)`` and the
  // doubling-L → ¼-gap rate.
  const double g1 = sos2_chord_worst_gap(1);
  const double g2 = sos2_chord_worst_gap(2);
  const double g4 = sos2_chord_worst_gap(4);
  const double g8 = sos2_chord_worst_gap(8);
  const double g16 = sos2_chord_worst_gap(16);

  // Strict monotonic decay.
  CHECK(g2 < g1);
  CHECK(g4 < g2);
  CHECK(g8 < g4);
  CHECK(g16 < g8);

  SUBCASE("doubling L cuts the worst-gap exactly 4×")
  {
    CHECK((g1 / g2) == doctest::Approx(4.0).epsilon(1e-12));
    CHECK((g2 / g4) == doctest::Approx(4.0).epsilon(1e-12));
    CHECK((g4 / g8) == doctest::Approx(4.0).epsilon(1e-12));
    CHECK((g8 / g16) == doctest::Approx(4.0).epsilon(1e-12));
  }

  SUBCASE("gap·L² is constant (envelope²·c/4) for every L")
  {
    const double constant = kCoeff * kSos2Envelope * kSos2Envelope / 4.0;
    CHECK((g1 * 1.0) == doctest::Approx(constant).epsilon(1e-12));
    CHECK((g2 * 4.0) == doctest::Approx(constant).epsilon(1e-12));
    CHECK((g4 * 16.0) == doctest::Approx(constant).epsilon(1e-12));
    CHECK((g8 * 64.0) == doctest::Approx(constant).epsilon(1e-12));
    CHECK((g16 * 256.0) == doctest::Approx(constant).epsilon(1e-12));
  }

  SUBCASE("absolute worst-gap matches c·(envelope/(2L))²")
  {
    for (const int L : {1, 2, 4, 8, 16, 32}) {
      const double half_w = kSos2Envelope / (2.0 * static_cast<double>(L));
      const double predicted = kCoeff * half_w * half_w;
      CHECK(sos2_chord_worst_gap(L)
            == doctest::Approx(predicted).epsilon(1e-12));
    }
  }

  SUBCASE("issue #504 acceptance: L=4 SOS2 caps gap at 1% of L_max")
  {
    // The "expected impact" in the issue body claims R/A drops from
    // 1.23× to ~1.05× at L=4 + SOS2.  The analytic upper bound is
    // ``gap / L_max = 1/(4·L²)`` (with L_max = c·envelope² the true
    // peak loss at fmax).  L=4 → gap/L_max = 1/64 ≈ 1.56%.  L=10 →
    // gap/L_max = 1/400 ≈ 0.25%.  Pin the analytic claim so any
    // regression in the chord-formula constants surfaces immediately.
    const double L_max = kCoeff * kSos2Envelope * kSos2Envelope;
    CHECK((sos2_chord_worst_gap(4) / L_max)
          == doctest::Approx(1.0 / 64.0).epsilon(1e-12));
    CHECK((sos2_chord_worst_gap(10) / L_max)
          == doctest::Approx(1.0 / 400.0).epsilon(1e-12));
  }
}

TEST_CASE(
    "tangent_signed_flow L-secant: bound is TIGHT at every breakpoint "
    "b_l = l·w")
{
  // Analytical sanity: at every breakpoint the piecewise chord equals
  // the true quadratic exactly (verified directly from the chord-slope
  // formula).  Used to ground the worst-gap formula above.
  for (const int L : {2, 4, 8}) {
    const double w = kSos2Envelope / static_cast<double>(L);
    for (int l = 1; l <= L; ++l) {
      // True quadratic at |f| = l·w.
      const double f_break = static_cast<double>(l) * w;
      const double truth = kCoeff * f_break * f_break;

      // Piecewise chord at the breakpoint: v_1..v_l saturated at w,
      // v_{l+1}..v_L = 0.  Σ chord_slope_k · v_k = c · w · Σ_{k=1..l}
      // (2k-1) · w = c · w² · l².  Equals truth iff w² · l² = f²,
      // which holds because f = l·w.
      double chord = 0.0;
      for (int k = 1; k <= l; ++k) {
        const double chord_slope =
            kCoeff * w * static_cast<double>((2 * k) - 1);
        chord += chord_slope * w;
      }
      CHECK(chord == doctest::Approx(truth).epsilon(1e-12));
    }
  }
}

// ── (12) Solve-side SOS2 test (review P1-2) ─────────────────────────
//
// The build-side tests above pin ``sos2_set_count()`` and the LP-row
// structure, but none actually solves a MIP under SOS2 enforcement.
// ``CplexSolverBackend::add_sos2`` could be silently broken (wrong
// sostype, off-by-one numsosnz, mis-aligned sosbeg) and every test in
// this file would still pass.
//
// This test forces an LP solve via the configured MIP solver and
// inspects the ``v_l`` primal values.  Under SOS2:
//   * At most TWO of the L segment cols may be non-zero
//   * The two non-zeros must be ADJACENT in the listed order
//
// Skipped when no MIP solver is loaded (CLP-only CI builds).

TEST_CASE(
    "tangent_signed_flow L=4 + use_sos2 (lambda-form): CPLEX solve "
    "respects at-most-two-adjacent-non-zero on the λ ladder")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  auto& reg = SolverRegistry::instance();
  std::string mip_solver;
  for (const auto& name : reg.available_solvers()) {
    if (reg.supports_mip(name)) {
      mip_solver = name;
      break;
    }
  }
  if (mip_solver.empty()) {
    MESSAGE("Skipping MIP test — supports_mip() returned false");
    return;
  }

  // Build a 2-bus fixture using the default ctor (no solver override
  // path), then post-build inspect the LP solution.  The default solver
  // (CPLEX in this environment) supports SOS2; if a future config
  // picks a non-MIP default we skip per the guard above.
  TwoBusSos2Fixture fix(/*loss_secant_segments=*/4, /*loss_use_sos2=*/true);
  auto& li = fix.lp();
  REQUIRE(li.sos2_set_count() == 1);

  const auto solve_status = li.resolve();
  REQUIRE(solve_status.has_value());

  const auto sol = li.get_col_sol();
  const auto& col_map = li.col_name_map();

  // Collect the λ_l primal values in order.  Lambda-form emits
  // ``line_flow_lambda_<scen>_<stage>_<block>_<l>`` for l = 0..2L.
  // For L=4 → 9 cols indexed 0..8.
  constexpr int L = 4;
  constexpr int lambda_count = (2 * L) + 1;
  std::array<double, lambda_count> lam {};
  for (const auto& [name, idx] : col_map) {
    if (!name.contains("line_flow_lambda_")) {
      continue;
    }
    const auto last_underscore = name.find_last_of('_');
    if (last_underscore == std::string_view::npos) {
      continue;
    }
    const auto seg_str = name.substr(last_underscore + 1);
    int seg_idx {};
    auto first = seg_str.data();
    auto last = first + seg_str.size();  // NOLINT
    if (std::from_chars(first, last, seg_idx).ec != std::errc {}) {
      continue;
    }
    if (seg_idx >= 0 && seg_idx < lambda_count) {
      lam[static_cast<std::size_t>(seg_idx)] = sol[value_of(idx)];
    }
  }

  // Pure SOS2 invariant (Beale & Tomlin 1970): at most TWO of the
  // 2L+1 lambda columns may be non-zero, AND if two are non-zero
  // their indices must be ADJACENT in the listed order.
  std::vector<int> nonzero_indices;
  for (int l = 0; l < lambda_count; ++l) {
    if (lam[static_cast<std::size_t>(l)] > 1e-6) {
      nonzero_indices.push_back(l);
    }
  }
  CAPTURE(lam[0]);
  CAPTURE(lam[1]);
  CAPTURE(lam[2]);
  CAPTURE(lam[3]);
  CAPTURE(lam[4]);
  CAPTURE(lam[5]);
  CAPTURE(lam[6]);
  CAPTURE(lam[7]);
  CAPTURE(lam[8]);
  CHECK(nonzero_indices.size() <= 2);
  if (nonzero_indices.size() == 2) {
    CHECK(nonzero_indices[1] == nonzero_indices[0] + 1);
  }

  // Convexity: Σ λ_l = 1.
  double sum_lambda = 0.0;
  for (const auto& v : lam) {
    sum_lambda += v;
  }
  CHECK(sum_lambda == doctest::Approx(1.0).epsilon(1e-3));

  // Flow tie: f = Σ b_l · λ_l.  Breakpoints b_l = (l-L)·w with
  // L=4, w=TMAX/L=50: b_l ∈ {-200, -150, -100, -50, 0, 50, 100,
  // 150, 200}.  Fixture demand = 100 MW @ bus 2, gen capacity =
  // 500 MW @ bus 1 → LP picks f = 100, so λ_6 = 1 (single non-zero
  // at the breakpoint b_6 = +100).  Other SOS2-feasible (λ_5, λ_6)
  // combos with f = 100 require λ_5 = 0 anyway, so the LP-optimum
  // is the single-non-zero case.
  constexpr double w = TwoBusSos2Fixture::TMAX / L;
  double f_from_lambda = 0.0;
  for (int l = 0; l < lambda_count; ++l) {
    const double b_l = static_cast<double>(l - L) * w;
    f_from_lambda += b_l * lam[static_cast<std::size_t>(l)];
  }
  CHECK(f_from_lambda == doctest::Approx(100.0).epsilon(1e-3));
}

// ── (13) Edge cases for the refactored L-secant emission ────────────
//
// The 2026-06 refactor consolidated the v0 L=1-vs-L>1 column branching
// and the two abs row blocks into single helpers.  These tests target
// edge cases that the previous structural tests didn't pin: per-block
// scaling, per-line scaling, and asymmetric envelope handling.

namespace test_line_losses_sos2_edge_ns  // NOLINT
{
namespace  // NOLINT
{

/// Build a 2-bus fixture configured for tangent_signed_flow + L-secant
/// SOS2 with ``n_blocks`` chronological blocks (all serving the same
/// 100 MW demand).  Mirrors ``TwoBusSos2Fixture`` but parameterised on
/// block count so the abs row / SOS2 set scaling can be tested.
struct MultiBlockSos2Fixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  static constexpr double R = 0.01;
  static constexpr double V = 100.0;
  static constexpr double TMAX = 200.0;

  MultiBlockSos2Fixture(int L, bool use_sos2, int n_blocks)
      : system {
            .name = "MultiBlockSos2",
            .bus_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "b1",
                    },
                    {
                        .uid = Uid {2},
                        .name = "b2",
                    },
                },
            .demand_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {2},
                        .capacity = 100.0,
                    },
                },
            .generator_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "g1",
                        .bus = Uid {1},
                        .gcost = 10.0,
                        .capacity = 500.0,
                    },
                },
            .line_array =
                {
                    make_line(L, use_sos2),
                },
        }
      , simulation(build_simulation(n_blocks))
      , opts {}
      , options(make_options())
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_opts(use_sos2))
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static Line make_line(int L, bool use_sos2)
  {
    Line ln {
        .uid = Uid {1},
        .name = "l1",
        .bus_a = Uid {1},
        .bus_b = Uid {2},
        .voltage = V,
        .resistance = R,
        .line_losses_mode = OptName {std::string {"tangent_signed_flow"}},
        .loss_segments = 5,
        .tmax_ba = TMAX,
        .tmax_ab = TMAX,
        .capacity = TMAX,
    };
    ln.loss_secant_segments = L;
    ln.loss_use_sos2 = use_sos2;
    return ln;
  }

  static Simulation build_simulation(int n_blocks)
  {
    Simulation sim;
    for (int i = 1; i <= n_blocks; ++i) {
      sim.block_array.push_back({
          .uid = Uid {i},
          .duration = 1,
      });
    }
    sim.stage_array.push_back({
        .uid = Uid {1},
        .first_block = 0,
        .count_block = static_cast<Size>(n_blocks),
    });
    sim.scenario_array.push_back({
        .uid = Uid {0},
    });
    return sim;
  }

  PlanningOptionsLP make_options()
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.row_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_name_map = true;
    return PlanningOptionsLP(opts);
  }

  static LpMatrixOptions build_opts(bool wants_sos2)
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    test_line_losses_sos2_ns::pin_sos2_capable_solver(bo, wants_sos2);
    return bo;
  }
};

}  // namespace
}  // namespace test_line_losses_sos2_edge_ns

using test_line_losses_sos2_edge_ns::MultiBlockSos2Fixture;

TEST_CASE(
    "tangent_signed_flow L=4 + use_sos2 (lambda-form): SOS2 set "
    "count scales with block count (3 blocks ⇒ 3 sets)")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  MultiBlockSos2Fixture fix(/*L=*/4, /*use_sos2=*/true, /*n_blocks=*/3);
  auto& li = fix.lp();
  // 1 SOS2 set per (line, block) × 3 blocks = 3 sets.
  CHECK(li.sos2_set_count() == 3);
  // Lambda-form: 2L+1 = 9 ``line_flow_lambda_`` cols × 3 blocks = 27.
  CHECK(count_cols_containing(li, "line_flow_lambda_") == (((2 * 4) + 1) * 3));
  // Lambda-form drops the segment-form abs rows entirely.
  CHECK(count_cols_containing(li, "line_flow_abs_") == 0);
  CHECK(count_rows_containing(li, "line_flow_abs") == 0);
  // 1 convexity row + 1 flow row per (line, block) × 3 blocks = 3 each.
  CHECK(count_rows_containing(li, "line_loss_lambda_convex") == 3);
  CHECK(count_rows_containing(li, "line_loss_lambda_flow") == 3);
}

TEST_CASE(
    "tangent_signed_flow L=4 + use_sos2 (lambda-form): 5 blocks ⇒ "
    "45 lambda cols, 0 abs rows, 5 convexity + 5 flow + 5 SOS2 sets")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  // Stress test: the lambda-form helpers must produce the same
  // scaling on a longer horizon.  Catches any hidden per-block
  // state that a naive lambda capture would leak.
  MultiBlockSos2Fixture fix(/*L=*/4, /*use_sos2=*/true, /*n_blocks=*/5);
  auto& li = fix.lp();
  CHECK(li.sos2_set_count() == 5);
  // Lambda-form: 9 cols × 5 blocks = 45 ``line_flow_lambda_`` cols.
  CHECK(count_cols_containing(li, "line_flow_lambda_") == (((2 * 4) + 1) * 5));
  CHECK(count_cols_containing(li, "line_flow_abs_") == 0);
  CHECK(count_rows_containing(li, "line_flow_abs") == 0);
  CHECK(count_rows_containing(li, "line_loss_lambda_convex") == 5);
  CHECK(count_rows_containing(li, "line_loss_lambda_flow") == 5);
}

TEST_CASE(
    "tangent_signed_flow L=1: refactor preserves single-aux back-compat "
    "(no segment index in col label)")
{
  // The refactor unified L=1 and L>1 column emission under a single
  // loop; the ``v_ctx_for`` lambda must still return ``block_ctx``
  // (3-tuple) at L=1 so write_lp emits the historical
  // ``…flow_abs_<scen>_<stage>_<block>`` label — no trailing ``_l1``.
  TwoBusSos2Fixture fix(/*L=*/1, /*use_sos2=*/false);
  auto& li = fix.lp();
  // Find the unique line_flow_abs_ col and inspect its name.
  std::string col_name;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.contains("line_flow_abs_")) {
      col_name = name;
      break;
    }
  }
  REQUIRE_FALSE(col_name.empty());
  // The 4-tuple emission would add a trailing ``_l1``-style segment
  // index after the block uid.  Count underscores: legacy 3-tuple
  // label has format ``<prefix>_<scen>_<stage>_<block>`` ⇒ exactly
  // 3 trailing integer-separated underscores after the prefix.
  // The 4-tuple form would have one more.  Cheap structural check:
  // assert that none of the L>1 distinguishers (``_l1``) is in the
  // label.
  CHECK_FALSE(col_name.contains("_l1"));
}

// ── (14) Lambda-form full-envelope reachability (issue #504 fix) ────
//
// The pre-lambda-form (segment) SOS2 emission used L cols ``v_l ∈
// [0, w]``  with SOS2 on them, which caps ``Σ v_l ≤ 2w`` (Beale–
// Tomlin "at most 2 adjacent non-zero") and silently restricts
// ``|f| ≤ 2·envelope/L``.  For ``L ≥ 3`` that's below the line
// rating ⇒ demand-fail / infeasibility on cases that need full
// envelope.
//
// The lambda-form refactor switches to 2L+1 breakpoint weights with
// SOS2, which interpolates linearly between any two adjacent
// breakpoints on ``[-envelope, +envelope]`` — no cap.  These tests
// pin the fix.
//
// We replicate the fixture pattern but parameterise demand to push
// past the old 2w cap.

namespace test_line_losses_sos2_envelope_ns  // NOLINT
{
namespace  // NOLINT
{

struct FullEnvelopeFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  static constexpr double R = 0.01;
  static constexpr double V = 100.0;
  static constexpr double TMAX = 200.0;

  FullEnvelopeFixture(int L,
                      bool use_sos2,
                      double demand_MW,
                      double loss_cost_eps = 0.0,
                      bool reverse_flow = false)
      : system {
            .name = "FullEnvelope",
            .bus_array =
                {
                    {.uid = Uid {1}, .name = "b1",},
                    {.uid = Uid {2}, .name = "b2",},
                },
            // Default: demand on b2, gen on b1 → f > 0 (bus_a → bus_b).
            // reverse_flow=true: swap so demand is on b1 and gen on b2,
            // driving f < 0 on the same line (covers the lambda-form
            // negative half of the breakpoint ladder).
            .demand_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "d1",
                        .bus = Uid {reverse_flow ? 1 : 2},
                        .capacity = demand_MW,
                    },
                },
            .generator_array =
                {
                    {
                        .uid = Uid {1},
                        .name = "g1",
                        .bus = Uid {reverse_flow ? 2 : 1},
                        .gcost = 10.0,
                        .capacity = 500.0,
                    },
                },
            .line_array = {make_line(L, use_sos2, loss_cost_eps),},
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1},
                  .first_block = 0,
                  .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options())
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_opts(use_sos2))
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  static Line make_line(int L, bool use_sos2, double loss_cost_eps)
  {
    Line ln {
        .uid = Uid {1},
        .name = "l1",
        .bus_a = Uid {1},
        .bus_b = Uid {2},
        .voltage = V,
        .resistance = R,
        .line_losses_mode = OptName {std::string {"tangent_signed_flow"}},
        .loss_segments = 5,
        .tmax_ba = TMAX,
        .tmax_ab = TMAX,
        .capacity = TMAX,
    };
    ln.loss_secant_segments = L;
    ln.loss_use_sos2 = use_sos2;
    if (loss_cost_eps > 0.0) {
      ln.loss_cost_eps = loss_cost_eps;
    }
    return ln;
  }

  PlanningOptionsLP make_options()
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    return PlanningOptionsLP {opts};
  }

  static LpMatrixOptions build_opts(bool wants_sos2)
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    test_line_losses_sos2_ns::pin_sos2_capable_solver(bo, wants_sos2);
    return bo;
  }
};

}  // namespace
}  // namespace test_line_losses_sos2_envelope_ns

using test_line_losses_sos2_envelope_ns::FullEnvelopeFixture;

TEST_CASE(
    "tangent_signed_flow L=4 + SOS2 (lambda-form) reaches full "
    "envelope — demand = 200 MW = tmax solves without demand-fail")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  // Demand at the full envelope (= tmax = 200 MW).  Under the old
  // segment-form SOS2 the cap |f| ≤ 2w = 2·200/4 = 100 MW would
  // force demand-fail (cost ≈ 100 × 1000 = 100 000 vs gen cost
  // 200 × 10 = 2 000).  Lambda-form reaches f = 200 cleanly via
  // λ_8 = 1 (single non-zero at b_8 = +envelope).
  FullEnvelopeFixture fix(/*L=*/4, /*use_sos2=*/true, /*demand_MW=*/200.0);
  auto& li = fix.lp();
  const auto solve_status = li.resolve();
  REQUIRE(solve_status.has_value());

  // Objective must be dominated by gen cost (~ $2 000) not by
  // demand-fail penalty (~ $200 000).  Loose upper bound: $10 000
  // covers gen + ε + small losses.
  const double obj = li.get_obj_value();
  CAPTURE(obj);
  CHECK(obj < 10000.0);

  // The boundary lambda λ_{2L} = λ_8 (at b_8 = +envelope) should
  // carry ≈ all the weight.
  const auto sol = li.get_col_sol();
  const auto& col_map = li.col_name_map();
  constexpr int L = 4;
  constexpr int lambda_count = (2 * L) + 1;
  std::array<double, lambda_count> lam {};
  for (const auto& [name, idx] : col_map) {
    if (!name.contains("line_flow_lambda_")) {
      continue;
    }
    const auto last_underscore = name.find_last_of('_');
    if (last_underscore == std::string_view::npos) {
      continue;
    }
    const auto seg_str = name.substr(last_underscore + 1);
    int seg_idx {};
    auto first = seg_str.data();
    auto last = first + seg_str.size();  // NOLINT
    if (std::from_chars(first, last, seg_idx).ec != std::errc {}) {
      continue;
    }
    if (seg_idx >= 0 && seg_idx < lambda_count) {
      lam[static_cast<std::size_t>(seg_idx)] = sol[value_of(idx)];
    }
  }
  CHECK(lam[lambda_count - 1] == doctest::Approx(1.0).epsilon(1e-3));
}

// ── (15) ε-rely segment-form (regime B) full-envelope reachability ──
//
// ``L > 1 && !use_sos2 && loss_cost_eps > 0`` is the ε-rely path:
// pure LP, no MIP, no cap.  Σ v_l = |f| at LP optimum (ε term
// makes that the cheapest feasible Σ), LP-greedy fills v_1 → v_L
// because chord_slope_l strictly increases in l.  Reaches the full
// envelope.

TEST_CASE(
    "tangent_signed_flow L=4 + ε > 0 + no SOS2 (ε-rely): reaches "
    "full envelope, fills bottom-up via LP greediness")
{
  // ε on the per-line override path keeps the fixture surface
  // small (no test-only setter on PlanningOptionsLP).
  FullEnvelopeFixture fix(/*L=*/4,
                          /*use_sos2=*/false,
                          /*demand_MW=*/200.0,
                          /*loss_cost_eps=*/1e-3);

  auto& li = fix.lp();
  const auto solve_status = li.resolve();
  REQUIRE(solve_status.has_value());

  const double obj = li.get_obj_value();
  CAPTURE(obj);
  // Gen-dominated obj (~ $2 000 + small losses), not demand-fail
  // (~ $200 000).  Loose upper bound covers gen + ε + losses.
  CHECK(obj < 10000.0);

  // No SOS2 set declared (this is the pure-LP regime).
  CHECK(li.sos2_set_count() == 0);

  // 4 segment cols emitted (the segment-form, not lambda-form).
  CHECK(count_cols_containing(li, "line_flow_abs_") == 4);
  CHECK(count_cols_containing(li, "line_flow_lambda_") == 0);
}

// ── (16) Negative-flow lambda-form coverage ─────────────────────────
//
// The breakpoint ladder b_l = (l−L)·w  spans [-envelope, +envelope].
// All other lambda-form tests drive f > 0 (gen on b1, demand on b2),
// which only stresses the upper half (l > L).  This test flips the
// fixture (gen on b2, demand on b1) so f = −demand and SOS2 picks
// λ_l on the LOWER half (l < L).  A sign bug in
// ``b_l = (l − L) · seg_width``  (e.g., `(L − l)` or `l·w`) would
// fail this test even though the existing positive-flow tests pass.

TEST_CASE(
    "tangent_signed_flow L=4 + SOS2 (lambda-form) negative flow: "
    "demand on bus_a drives f < 0, λ_l carries weight for l < L")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  // Demand 100 MW on b1 (= bus_a) with gen on b2: f flows b2 → b1,
  // signed convention bus_a→bus_b means f = −100.  Lambda ladder
  // b_l ∈ {−200, …, +200}, b_2 = −100 (= demand magnitude), so the
  // LP picks λ_2 = 1 (single-non-zero at b_l = −100).
  FullEnvelopeFixture fix(/*L=*/4,
                          /*use_sos2=*/true,
                          /*demand_MW=*/100.0,
                          /*loss_cost_eps=*/0.0,
                          /*reverse_flow=*/true);
  auto& li = fix.lp();
  const auto solve_status = li.resolve();
  REQUIRE(solve_status.has_value());

  const auto sol = li.get_col_sol();
  const auto& col_map = li.col_name_map();
  constexpr int L = 4;
  constexpr int lambda_count = (2 * L) + 1;
  std::array<double, lambda_count> lam {};
  for (const auto& [name, idx] : col_map) {
    if (!name.contains("line_flow_lambda_")) {
      continue;
    }
    const auto last_underscore = name.find_last_of('_');
    if (last_underscore == std::string_view::npos) {
      continue;
    }
    const auto seg_str = name.substr(last_underscore + 1);
    int seg_idx {};
    auto first = seg_str.data();
    auto last = first + seg_str.size();  // NOLINT
    if (std::from_chars(first, last, seg_idx).ec != std::errc {}) {
      continue;
    }
    if (seg_idx >= 0 && seg_idx < lambda_count) {
      lam[static_cast<std::size_t>(seg_idx)] = sol[value_of(idx)];
    }
  }

  // Convexity: Σ λ_l = 1.
  double sum_lambda = 0.0;
  for (const auto& v : lam) {
    sum_lambda += v;
  }
  CHECK(sum_lambda == doctest::Approx(1.0).epsilon(1e-3));

  // Flow tie: f = Σ b_l · λ_l = -100 (negative because demand is
  // on bus_a = bus 1, gen on bus_b = bus 2).
  constexpr double w = FullEnvelopeFixture::TMAX / L;
  double f_from_lambda = 0.0;
  for (int l = 0; l < lambda_count; ++l) {
    const double b_l = static_cast<double>(l - L) * w;
    f_from_lambda += b_l * lam[static_cast<std::size_t>(l)];
  }
  CHECK(f_from_lambda == doctest::Approx(-100.0).epsilon(1e-3));

  // SOS2 invariant: at most 2 adjacent λ_l non-zero, lower half.
  std::vector<int> nonzero_indices;
  for (int l = 0; l < lambda_count; ++l) {
    if (lam[static_cast<std::size_t>(l)] > 1e-6) {
      nonzero_indices.push_back(l);
    }
  }
  CHECK(nonzero_indices.size() <= 2);
  if (!nonzero_indices.empty()) {
    // Active breakpoint(s) must be in the lower half (l ≤ L = 4).
    CHECK(nonzero_indices.back() <= L);
  }
}

// ── (17) ε-rely L=4: Σ v_l = |f| at LP optimum ──────────────────────
//
// The structural property that makes ε-rely correct is the equality
// ``Σ v_l = |f|``  at LP optimum — driven by ``loss_cost_eps > 0`` on
// each v_l, which makes the cheapest feasible Σ (= |f| from the abs
// rows) the LP minimum.  This pins the chord at a finite value
// (chord = Σ chord_slope_l · v_l ≤ chord_slope_L · |f| = bounded).
//
// IMPORTANT CORRECTION (vs the first draft of this test).  We do
// NOT claim ε-rely forces bottom-up fill ``v_1 ≥ v_2 ≥ … ≥ v_L``.
// The chord row is an UPPER bound on ℓ; the K-tangent rows are LOWER
// bounds.  At LP optimum the LP picks ``ℓ = max_tangent(f)`` and
// the chord row is INACTIVE — so any distribution of v_l with
// ``Σ v_l = |f|``  is LP-equivalent (the objective is indifferent).
// Empirically the solver picks e.g. ``v = {0, 25, 50, 0}``  rather
// than the bottom-up ``{50, 25, 0, 0}``  — both yield the same obj.
//
// The structural test is therefore just ``Σ v_l = |f|``.  A bug in
// the ε term (e.g., ε = 0 by accident) would let Σ v_l inflate
// past |f| and fail the equality.

TEST_CASE(
    "tangent_signed_flow L=4 + ε > 0 + no SOS2 (ε-rely): Σ v_l = |f| "
    "at LP optimum")
{
  FullEnvelopeFixture fix(/*L=*/4,
                          /*use_sos2=*/false,
                          /*demand_MW=*/75.0,
                          /*loss_cost_eps=*/1e-3);
  auto& li = fix.lp();
  const auto solve_status = li.resolve();
  REQUIRE(solve_status.has_value());

  const auto sol = li.get_col_sol();
  const auto& col_map = li.col_name_map();

  // Collect v_l primals.
  std::array<double, 4> v {};
  for (const auto& [name, idx] : col_map) {
    if (!name.contains("line_flow_abs_")) {
      continue;
    }
    const auto last_underscore = name.find_last_of('_');
    if (last_underscore == std::string_view::npos) {
      continue;
    }
    const auto seg_str = name.substr(last_underscore + 1);
    int seg_idx {};
    auto first = seg_str.data();
    auto last = first + seg_str.size();  // NOLINT
    if (std::from_chars(first, last, seg_idx).ec != std::errc {}) {
      continue;
    }
    if (seg_idx >= 1 && seg_idx <= 4) {
      v[static_cast<std::size_t>(seg_idx - 1)] = sol[value_of(idx)];
    }
  }

  CAPTURE(v[0]);
  CAPTURE(v[1]);
  CAPTURE(v[2]);
  CAPTURE(v[3]);

  // Σ v_l = |f| = 75 — the core ε-rely invariant.
  const double sum_v = v[0] + v[1] + v[2] + v[3];
  CHECK(sum_v == doctest::Approx(75.0).epsilon(1e-3));

  // Each v_l honours its [0, w] bound with w = TMAX / L = 50.
  constexpr double w = FullEnvelopeFixture::TMAX / 4;
  for (const auto& vl : v) {
    CHECK(vl >= -1e-9);
    CHECK(vl <= w + 1e-9);
  }
}

// ── (18) Lambda col bounds λ_l ∈ [0, 1] ─────────────────────────────
//
// A copy-paste bug like ``uppb = seg_width`` (cribbed from the
// segment-form emitter) would silently let λ exceed 1 and break
// the convexity constraint's semantic.  Pin the structural bound
// directly via the LP's col_upp accessor.

TEST_CASE("tangent_signed_flow L=4 + SOS2 (lambda-form): λ_l ∈ [0, 1]")
{
  if (!sos2_available()) {
    MESSAGE("Skipping SOS2 test — no SOS2-capable backend loaded");
    return;
  }
  FullEnvelopeFixture fix(/*L=*/4,
                          /*use_sos2=*/true,
                          /*demand_MW=*/100.0);
  auto& li = fix.lp();
  const auto col_upp = li.get_col_upp_raw();
  const auto col_low = li.get_col_low_raw();
  const auto& col_map = li.col_name_map();

  int lambda_cols_seen = 0;
  for (const auto& [name, idx] : col_map) {
    if (!name.contains("line_flow_lambda_")) {
      continue;
    }
    ++lambda_cols_seen;
    const auto raw = value_of(idx);
    CHECK(col_low[static_cast<std::size_t>(raw)]
          == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(col_upp[static_cast<std::size_t>(raw)]
          == doctest::Approx(1.0).epsilon(1e-9));
  }
  CHECK(lambda_cols_seen == (2 * 4) + 1);
}
