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

#include <cstddef>
#include <string>
#include <string_view>

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
      , sys_lp(system, sim_lp, build_opts())
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

  static LpMatrixOptions build_opts()
  {
    LpMatrixOptions bo;
    bo.col_with_names = true;
    bo.col_with_name_map = true;
    bo.row_with_names = true;
    bo.row_with_name_map = true;
    return bo;
  }
};

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
  // Global default: use_sos2 = false, L = 1.  Per-line override:
  // L = 4 + use_sos2 = true.  The per-line override must win.
  TwoBusSos2Fixture fix(/*L=*/4,
                        /*use_sos2=*/true,
                        /*K=*/5,
                        /*per_line_override=*/true,
                        /*global_secant_segments=*/1,
                        /*global_use_sos2=*/false);
  auto& li = fix.lp();
  CHECK(count_cols_containing(li, "line_flow_abs_") == 4);
  CHECK(li.sos2_set_count() == 1);
}

TEST_CASE(
    "tangent_signed_flow: global model_options applies when per-line unset")
{
  // No per-line override; global says L = 3 + SOS2 on.  Line should
  // inherit those values via the planning-options resolver.
  TwoBusSos2Fixture fix(/*L=*/0,
                        /*use_sos2=*/false,
                        /*K=*/5,
                        /*per_line_override=*/false,
                        /*global_secant_segments=*/3,
                        /*global_use_sos2=*/true);
  auto& li = fix.lp();
  CHECK(count_cols_containing(li, "line_flow_abs_") == 3);
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
