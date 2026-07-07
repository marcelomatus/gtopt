// SPDX-License-Identifier: BSD-3-Clause
//
// test_line_flow_always_bounded.cpp — LOCK the "line flow column is
// never free" invariant.
//
// ``Line.enforce_level`` was RETIRED (2026-06-10): ``source/
// line_lp.cpp`` now passes ``enforce_capacity = true`` unconditionally,
// restoring the original pre-2026-05-22 behaviour where every line
// flow column is bound by ``tmax`` and is NEVER left free.  The
// (ignored) ``enforce_level = 0`` flag no longer frees the column.
//
// This test pins that invariant for BOTH flow representations on a
// line with ``enforce_level = 0`` + explicit ``tmax`` + NO
// ``capacity`` (so the only source of a finite bound is ``tmax``):
//
//   * two-variable (``piecewise`` / ``linear``): ``flowp`` and
//     ``flown`` are separate non-negative columns.  ``uppb(flowp) ≈
//     tmax_ab``, ``uppb(flown) ≈ tmax_ba``, ``lowb(flowp) ≈ 0`` — all
//     finite and well below solver-infinity.
//   * single-variable signed (``tangent_signed_flow``): one ``flows``
//     column over ``[−tmax_ba, +tmax_ab]`` — both bounds finite.
//
// A future regression that frees a flow column (e.g. resurrecting the
// ``enforce_level = 0`` relaxation) would push one of these bounds to
// the solver infinity and fail loudly here.

#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/line.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;

// Wrap the file body in a uniquely-named non-anonymous outer namespace
// so the Unity build cannot collide with same-named helpers in sibling
// test files (see CLAUDE.md ``unity-anon-namespace`` note).
namespace test_line_flow_always_bounded_ns
{

namespace
{

// Any column upper/lower bound whose magnitude reaches this threshold
// is "free" (at solver infinity).  CPLEX/CLP infinity is ~1e30; the
// scaled-view physical bounds round-trip to ~1e20 after equilibration.
// A finite tmax of a few hundred MW is many orders below this.
constexpr double kFreeThreshold = 1e18;

constexpr double kTmaxAb = 137.0;  // asymmetric on purpose so a swap is
constexpr double kTmaxBa = 211.0;  // visible if the bounds get crossed.

/// Locate the single LP column whose name contains ``substr`` but NOT
/// ``exclude`` (empty ``exclude`` disables the filter).  The exclude
/// filter lets us pick the ``line_flowp_`` aggregator without also
/// matching the per-segment ``line_flowp_seg_k_`` columns the
/// piecewise loss model emits.  Requires the col name map to be
/// populated (enabled via ``lp_matrix_options`` below).
[[nodiscard]] auto find_col(const LinearInterface& li,
                            std::string_view substr,
                            std::string_view exclude = {}) -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (!name.contains(substr)) {
      continue;
    }
    if (!exclude.empty() && name.contains(exclude)) {
      continue;
    }
    found = idx;
    ++count;
  }
  REQUIRE(count == 1);
  return found;
}

/// Build a 2-bus / single-line / single-block LP in the requested
/// loss mode.  The line carries ``enforce_level = 0`` + explicit
/// ``tmax_ab`` / ``tmax_ba`` and NO ``capacity`` — so the ONLY finite
/// bound the flow column can pick up is ``tmax``.
struct OneLineFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit OneLineFixture(std::string_view mode_name, int loss_segments = 4)
      : system {
            .name = "FlowAlwaysBounded",
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
                        .capacity = 50.0,
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
                    {
                        .uid = Uid {1},
                        .name = "l1",
                        .bus_a = Uid {1},
                        .bus_b = Uid {2},
                        .voltage = 100.0,
                        .resistance = 0.01,
                        .line_losses_mode =
                            OptName {std::string(mode_name)},
                        .loss_segments = loss_segments,
                        // Explicit, asymmetric tmax; NO capacity field.
                        .tmax_ba = kTmaxBa,
                        .tmax_ab = kTmaxAb,
                        // RETIRED no-op flag — explicitly 0 to prove it
                        // does NOT free the flow column.
                        .enforce_level = OptInt {0},
                    },
                },
        }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1,},},
            .stage_array =
                {{.uid = Uid {1}, .first_block = 0, .count_block = 1,},},
            .scenario_array = {{.uid = Uid {0},},},
        }
      , opts {}
      , options(make_options())
      , sim_lp(simulation, options)
      , sys_lp(system, sim_lp, build_opts())
  {
  }

  [[nodiscard]] auto& lp() { return sys_lp.linear_interface(); }

private:
  PlanningOptionsLP make_options()
  {
    opts.model_options.use_single_bus = false;
    opts.model_options.use_kirchhoff = false;
    opts.model_options.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
    opts.lp_matrix_options.col_with_names = true;
    opts.lp_matrix_options.col_with_name_map = true;
    opts.lp_matrix_options.row_with_names = true;
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

}  // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_CASE(
    "Line flow column never free — two-variable flowp/flown bounded by tmax")
{
  // ``piecewise`` (and ``linear``) build two non-negative directional
  // columns: ``flowp`` (A→B) and ``flown`` (B→A).  Each must be bound
  // above by its directional ``tmax`` and below by 0.  With
  // ``enforce_level = 0`` and no ``capacity``, ``tmax`` is the only
  // bound — proving the flag no longer frees the column.
  for (const std::string_view mode : {"piecewise", "linear"}) {
    CAPTURE(mode);
    OneLineFixture fix(mode);
    auto& li = fix.lp();

    // Exclude the per-segment ``line_flowp_seg_k_`` columns piecewise
    // emits — we want the directional aggregator column.
    const auto fp = find_col(li, "line_flowp_", "_seg_");
    const auto fn = find_col(li, "line_flown_", "_seg_");

    const double upp_fp = li.get_col_upp()[value_of(fp)];
    const double upp_fn = li.get_col_upp()[value_of(fn)];
    const double low_fp = li.get_col_low()[value_of(fp)];

    CAPTURE(upp_fp);
    CAPTURE(upp_fn);
    CAPTURE(low_fp);

    // Upper bounds finite and at the directional tmax.
    CHECK(upp_fp == doctest::Approx(kTmaxAb));
    CHECK(upp_fn == doctest::Approx(kTmaxBa));
    CHECK(upp_fp < kFreeThreshold);
    CHECK(upp_fn < kFreeThreshold);

    // Lower bound pinned at 0 (non-negative directional flow).
    CHECK(low_fp == doctest::Approx(0.0));
  }
}

TEST_CASE(
    "Line flow column never free — signed flows column bounded "
    "[-tmax_ba, +tmax_ab]")
{
  // ``tangent_signed_flow`` builds ONE signed column over
  // ``[−tmax_ba, +tmax_ab]``.  Both bounds must be finite — a free
  // column on either side would be the regression this test guards.
  OneLineFixture fix("tangent_signed_flow");
  auto& li = fix.lp();

  const auto fs = find_col(li, "line_flows_");
  const double low_fs = li.get_col_low()[value_of(fs)];
  const double upp_fs = li.get_col_upp()[value_of(fs)];

  CAPTURE(low_fs);
  CAPTURE(upp_fs);

  // Signed bounds: lower at −tmax_ba, upper at +tmax_ab; both finite.
  CHECK(low_fs == doctest::Approx(-kTmaxBa));
  CHECK(upp_fs == doctest::Approx(+kTmaxAb));
  CHECK(low_fs > -kFreeThreshold);
  CHECK(upp_fs < kFreeThreshold);
}

// NOLINTEND(bugprone-unchecked-optional-access)

}  // namespace test_line_flow_always_bounded_ns
