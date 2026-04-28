// SPDX-License-Identifier: BSD-3-Clause

/// @file test_line_losses.hpp
/// @brief Unit tests for the modular line losses engine.

#include <doctest/doctest.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/line.hpp>
#include <gtopt/line_losses.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/planning.hpp>
#include <gtopt/planning_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/system_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ─── LineLossesMode enum ────────────────────────────────────────────

TEST_CASE("LineLossesMode enum parsing")
{
  SUBCASE("round-trip all values")
  {
    for (const auto& entry : line_losses_mode_entries) {
      const auto parsed = enum_from_name<LineLossesMode>(entry.name);
      REQUIRE(parsed.has_value());
      CHECK(*parsed == entry.value);
      CHECK(enum_name(*parsed) == entry.name);
    }
  }

  SUBCASE("invalid name returns nullopt")
  {
    CHECK_FALSE(enum_from_name<LineLossesMode>("invalid").has_value());
    CHECK_FALSE(enum_from_name<LineLossesMode>("").has_value());
  }
}

// ─── Line::line_losses_mode_enum() ──────────────────────────────────

TEST_CASE("Line line_losses_mode_enum accessor")
{
  SUBCASE("default is nullopt (inherit from global)")
  {
    Line line;
    CHECK_FALSE(line.line_losses_mode_enum().has_value());
  }

  SUBCASE("parses valid values")
  {
    Line line;
    line.line_losses_mode = "piecewise";
    auto m = line.line_losses_mode_enum();
    REQUIRE(m.has_value());
    CHECK(*m == LineLossesMode::piecewise);
  }

  SUBCASE("invalid value returns nullopt")
  {
    Line line;
    line.line_losses_mode = "invalid_mode";
    CHECK_FALSE(line.line_losses_mode_enum().has_value());
  }
}

// ─── resolve_mode ───────────────────────────────────────────────────

TEST_CASE("line_losses::resolve_mode fallback chain")
{
  PlanningOptions opts;
  // Global default is adaptive (from
  // PlanningOptionsLP::default_line_losses_mode)
  const PlanningOptionsLP options_lp(opts);

  SUBCASE("per-line explicit mode takes priority")
  {
    Line line;
    line.line_losses_mode = "bidirectional";
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::bidirectional);
  }

  SUBCASE("per-line use_line_losses=false maps to none")
  {
    Line line;
    line.use_line_losses = false;
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::none);
  }

  SUBCASE("per-line use_line_losses=true inherits global mode")
  {
    Line line;
    line.use_line_losses = true;
    // Global default is adaptive → resolves to piecewise (no expansion).
    // `adaptive` picks the smallest-LP PWL model, so piecewise_direct is
    // opt-in only.
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::piecewise);
  }

  SUBCASE("adaptive resolves to piecewise without expansion")
  {
    Line line;
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::piecewise);
  }

  SUBCASE("adaptive resolves to bidirectional with expansion")
  {
    Line line;
    CHECK(line_losses::resolve_mode(line, options_lp, true)
          == LineLossesMode::bidirectional);
  }

  SUBCASE("piecewise_direct + expansion demotes to piecewise")
  {
    Line line;
    line.line_losses_mode = "piecewise_direct";
    CHECK(line_losses::resolve_mode(line, options_lp, true)
          == LineLossesMode::piecewise);
  }

  SUBCASE("piecewise_direct + no expansion stays direct")
  {
    Line line;
    line.line_losses_mode = "piecewise_direct";
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::piecewise_direct);
  }

  SUBCASE("dynamic falls back to piecewise")
  {
    Line line;
    line.line_losses_mode = "dynamic";
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::piecewise);
  }

  SUBCASE("explicit line mode overrides use_line_losses")
  {
    Line line;
    line.use_line_losses = false;
    line.line_losses_mode = "linear";
    // Explicit mode wins over deprecated bool
    CHECK(line_losses::resolve_mode(line, options_lp, false)
          == LineLossesMode::linear);
  }

  SUBCASE(
      "per-line use_line_losses=true with global none falls back to default")
  {
    PlanningOptions opts_none;
    opts_none.model_options.line_losses_mode = OptName {"none"};
    opts_none.model_options.demand_fail_cost = 1000.0;
    const PlanningOptionsLP options_none(opts_none);

    Line line;
    line.use_line_losses = true;
    // Global is none, but per-line enables → falls back to default (adaptive)
    // Without expansion → piecewise (smallest-LP PWL)
    CHECK(line_losses::resolve_mode(line, options_none, false)
          == LineLossesMode::piecewise);
    // With expansion → bidirectional
    CHECK(line_losses::resolve_mode(line, options_none, true)
          == LineLossesMode::bidirectional);
  }
}

// ─── make_config ────────────────────────────────────────────────────

TEST_CASE("line_losses::make_config validates and auto-computes")
{
  Line line;

  SUBCASE("linear mode with explicit lossfactor")
  {
    auto cfg = line_losses::make_config(LineLossesMode::linear,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0.05,
                                        /*resistance=*/0,
                                        /*voltage=*/0,
                                        /*loss_segments=*/1,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::linear);
    CHECK(cfg.lossfactor == doctest::Approx(0.05));
  }

  SUBCASE("linear mode auto-computes lossfactor from R/V")
  {
    // λ = R · f_max / V² = 0.01 · 200 / 100² = 0.0002
    auto cfg = line_losses::make_config(LineLossesMode::linear,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0,
                                        /*resistance=*/0.01,
                                        /*voltage=*/100,
                                        /*loss_segments=*/1,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::linear);
    CHECK(cfg.lossfactor == doctest::Approx(0.0002));
  }

  SUBCASE("linear mode falls back to none without R/V")
  {
    auto cfg = line_losses::make_config(LineLossesMode::linear,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0,
                                        /*resistance=*/0,
                                        /*voltage=*/0,
                                        /*loss_segments=*/1,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::none);
  }

  SUBCASE("piecewise falls back to linear if nseg < 2")
  {
    auto cfg = line_losses::make_config(LineLossesMode::piecewise,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0.05,
                                        /*resistance=*/0.01,
                                        /*voltage=*/100,
                                        /*loss_segments=*/1,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::linear);
    CHECK(cfg.lossfactor == doctest::Approx(0.05));
  }

  SUBCASE("piecewise with valid params stays piecewise")
  {
    auto cfg = line_losses::make_config(LineLossesMode::piecewise,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0,
                                        /*resistance=*/0.01,
                                        /*voltage=*/100,
                                        /*loss_segments=*/3,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::piecewise);
    CHECK(cfg.nseg == 3);
    CHECK(cfg.resistance == doctest::Approx(0.01));
    CHECK(cfg.V2 == doctest::Approx(10000.0));
  }

  SUBCASE("bidirectional falls back to none without R/V/lossfactor")
  {
    auto cfg = line_losses::make_config(LineLossesMode::bidirectional,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0,
                                        /*resistance=*/0,
                                        /*voltage=*/0,
                                        /*loss_segments=*/3,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::none);
  }

  SUBCASE("bidirectional with valid params stays bidirectional")
  {
    auto cfg = line_losses::make_config(LineLossesMode::bidirectional,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0,
                                        /*resistance=*/0.01,
                                        /*voltage=*/100,
                                        /*loss_segments=*/3,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::bidirectional);
    CHECK(cfg.nseg == 3);
    CHECK(cfg.resistance == doctest::Approx(0.01));
    CHECK(cfg.V2 == doctest::Approx(10000.0));
  }

  SUBCASE("bidirectional with lossfactor but no R/V falls back to linear")
  {
    auto cfg = line_losses::make_config(LineLossesMode::bidirectional,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0.05,
                                        /*resistance=*/0,
                                        /*voltage=*/0,
                                        /*loss_segments=*/3,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::linear);
    CHECK(cfg.lossfactor == doctest::Approx(0.05));
  }

  SUBCASE("piecewise with nseg<2 but lossfactor falls back to linear")
  {
    auto cfg = line_losses::make_config(LineLossesMode::piecewise,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0.03,
                                        /*resistance=*/0,
                                        /*voltage=*/0,
                                        /*loss_segments=*/1,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::linear);
    CHECK(cfg.lossfactor == doctest::Approx(0.03));
  }

  SUBCASE("none mode stays none regardless of parameters")
  {
    auto cfg = line_losses::make_config(LineLossesMode::none,
                                        line,
                                        LossAllocationMode::receiver,
                                        /*lossfactor=*/0.05,
                                        /*resistance=*/0.01,
                                        /*voltage=*/100,
                                        /*loss_segments=*/3,
                                        /*fmax=*/200);
    CHECK(cfg.mode == LineLossesMode::none);
  }
}

// ─── Integration: full LP solve per mode ────────────────────────────

/// Helper: build a 2-bus system with one line configured for the given
/// losses mode and solve it.  Generator on bus 1 (cost 10), demand 100
/// on bus 2, line capacity 200, R=0.01Ω, V=100kV.
static auto solve_with_mode(std::string_view mode_name, int loss_segments = 3)
    -> double
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 100.0,
      },
  };

  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .line_losses_mode = OptName {std::string(mode_name)},
          .loss_segments = loss_segments,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  PlanningOptions opts;
  opts.use_single_bus = false;
  opts.use_kirchhoff = false;
  opts.scale_objective = 1000.0;
  opts.model_options.demand_fail_cost = 1000.0;

  const System system = {
      .name = "LossEngineTest",
      .bus_array = bus_array,
      .demand_array = demand_array,
      .generator_array = generator_array,
      .line_array = line_array,
  };

  const PlanningOptionsLP options(opts);
  SimulationLP sim_lp(simulation, options);
  SystemLP sys_lp(system, sim_lp);

  auto&& lp = sys_lp.linear_interface();
  auto result = lp.resolve();
  REQUIRE(result.has_value());
  CHECK(result.value() == 0);
  return lp.get_obj_value_raw();
}

TEST_CASE("line_losses engine - none mode produces zero-loss objective")
{
  // gen=100MW at $10, obj = 100*10/1000 = 1.0 (no losses)
  const auto obj = solve_with_mode("none");
  CHECK(obj == doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE("line_losses engine - linear mode produces small loss overhead")
{
  // With auto-computed lossfactor = R*fmax/V² = 0.01*200/10000 = 0.0002
  // Very small loss → obj just slightly above 1.0
  const auto obj = solve_with_mode("linear");
  CHECK(obj > 1.0);
  CHECK(obj < 1.001);
}

TEST_CASE("line_losses engine - piecewise mode produces loss overhead")
{
  // PWL approximation of R*f²/V² with 3 segments
  const auto obj = solve_with_mode("piecewise");
  CHECK(obj > 1.0);
  CHECK(obj < 1.01);
}

TEST_CASE("line_losses engine - bidirectional mode produces loss overhead")
{
  // Same quadratic loss but with two-direction decomposition
  const auto obj = solve_with_mode("bidirectional");
  CHECK(obj > 1.0);
  CHECK(obj < 1.01);
}

TEST_CASE(
    "line_losses engine - piecewise and bidirectional produce similar "
    "objectives")
{
  const auto obj_pw = solve_with_mode("piecewise");
  const auto obj_bi = solve_with_mode("bidirectional");
  // Both approximate the same quadratic loss; should agree closely.
  CHECK(obj_pw == doctest::Approx(obj_bi).epsilon(0.01));
}

TEST_CASE("line_losses engine - adaptive mode selects based on expansion")
{
  // Without expansion → piecewise.  We can test this indirectly:
  // adaptive with no expcap should match piecewise objective.
  const auto obj_adaptive = solve_with_mode("adaptive");
  const auto obj_piecewise = solve_with_mode("piecewise");
  CHECK(obj_adaptive == doctest::Approx(obj_piecewise).epsilon(0.001));
}

TEST_CASE("line_losses engine - global model_options.line_losses_mode")
{
  const Array<Bus> bus_array = {
      {.uid = Uid {1}, .name = "b1"},
      {.uid = Uid {2}, .name = "b2"},
  };

  const Array<Generator> generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .gcost = 10.0,
          .capacity = 500.0,
      },
  };

  const Array<Demand> demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {2},
          .capacity = 100.0,
      },
  };

  // Line has NO per-line mode → inherits from global.
  const Array<Line> line_array = {
      {
          .uid = Uid {1},
          .name = "l1",
          .bus_a = Uid {1},
          .bus_b = Uid {2},
          .voltage = 100.0,
          .resistance = 0.01,
          .loss_segments = 3,
          .tmax_ba = 200.0,
          .tmax_ab = 200.0,
          .capacity = 200.0,
      },
  };

  const Simulation simulation = {
      .block_array = {{.uid = Uid {1}, .duration = 1}},
      .stage_array = {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
      .scenario_array = {{.uid = Uid {0}}},
  };

  SUBCASE("global none → no losses")
  {
    PlanningOptions opts;
    opts.use_single_bus = false;
    opts.use_kirchhoff = false;
    opts.scale_objective = 1000.0;
    opts.model_options.line_losses_mode = OptName {"none"};
    opts.model_options.demand_fail_cost = 1000.0;

    const System system = {
        .name = "GlobalNone",
        .bus_array = bus_array,
        .demand_array = demand_array,
        .generator_array = generator_array,
        .line_array = line_array,
    };

    const PlanningOptionsLP options(opts);
    SimulationLP sim_lp(simulation, options);
    SystemLP sys_lp(system, sim_lp);
    auto&& lp = sys_lp.linear_interface();
    auto result = lp.resolve();
    REQUIRE(result.has_value());
    CHECK(lp.get_obj_value_raw() == doctest::Approx(1.0).epsilon(0.001));
  }
}

// ─── LP structure tests per mode ────────────────────────────────────

/// Helper: build a 2-bus system with the given line losses mode and
/// return (SystemLP, LinearInterface&) so callers can inspect the LP.
/// All names enabled so col/row names are available.
struct LPFixture
{
  System system;
  Simulation simulation;
  PlanningOptions opts;
  PlanningOptionsLP options;
  SimulationLP sim_lp;
  SystemLP sys_lp;

  explicit LPFixture(std::string_view mode_name,
                     int loss_segments = 3,
                     double R = 0.01,
                     double V = 100.0,
                     double lossfactor = 0.0,
                     double tmax = 200.0)
      : system {
          .name = "LPStructureTest",
          .bus_array =
              {
                  {.uid = Uid {1}, .name = "b1"},
                  {.uid = Uid {2}, .name = "b2"},
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
                  {
                      .uid = Uid {1},
                      .name = "l1",
                      .bus_a = Uid {1},
                      .bus_b = Uid {2},
                      .voltage = V,
                      .resistance = R,
                      .lossfactor = lossfactor,
                      .line_losses_mode = OptName {std::string(mode_name)},
                      .loss_segments = loss_segments,
                      .tmax_ba = tmax,
                      .tmax_ab = tmax,
                      .capacity = tmax,
                  },
              },
      }
      , simulation {
            .block_array = {{.uid = Uid {1}, .duration = 1}},
            .stage_array =
                {{.uid = Uid {1}, .first_block = 0, .count_block = 1}},
            .scenario_array = {{.uid = Uid {0}}},
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
    opts.use_single_bus = false;
    opts.use_kirchhoff = false;
    opts.scale_objective = 1000.0;
    opts.model_options.demand_fail_cost = 1000.0;
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

/// Count columns whose name contains the given substring.
static auto count_cols_containing(const LinearInterface& li,
                                  std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.col_name_map()) {
    if (name.find(substr) != std::string::npos) {
      ++count;
    }
  }
  return count;
}

/// Count rows whose name contains the given substring.
static auto count_rows_containing(const LinearInterface& li,
                                  std::string_view substr) -> int
{
  int count = 0;
  for (const auto& [name, _idx] : li.row_name_map()) {
    if (name.find(substr) != std::string::npos) {
      ++count;
    }
  }
  return count;
}

/// Find the single column whose name contains `substr`; require exactly one.
static auto find_col(const LinearInterface& li, std::string_view substr)
    -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.find(substr) != std::string::npos) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

/// Find column containing `substr` but NOT `exclude`; require exactly one.
static auto find_col(const LinearInterface& li,
                     std::string_view substr,
                     std::string_view exclude) -> ColIndex
{
  ColIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.find(substr) != std::string::npos
        && name.find(exclude) == std::string::npos)
    {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

/// Find the single row whose name contains `substr`; require exactly one.
static auto find_row(const LinearInterface& li, std::string_view substr)
    -> RowIndex
{
  RowIndex found {-1};
  int count = 0;
  for (const auto& [name, idx] : li.row_name_map()) {
    if (name.find(substr) != std::string::npos) {
      found = idx;
      ++count;
    }
  }
  REQUIRE(count == 1);
  return found;
}

// ── none mode LP structure ─────────────────────────────────────────

TEST_CASE("line_losses LP structure - none mode")
{
  LPFixture fix("none");
  auto& li = fix.lp();

  SUBCASE("single bidirectional flow variable, no loss variables")
  {
    // "none" creates 1 flow col (fp) with lowb = -tmax_ba, uppb = tmax_ab
    CHECK(count_cols_containing(li, "line_flowp_") == 1);
    CHECK(count_cols_containing(li, "line_flown_") == 0);
    CHECK(count_cols_containing(li, "line_lossp_") == 0);
  }

  SUBCASE("flow variable bounds are [-tmax_ba, +tmax_ab]")
  {
    const auto fp = find_col(li, "line_flowp_");
    CHECK(li.get_col_low()[value_of(fp)] == doctest::Approx(-200.0));
    CHECK(li.get_col_upp()[value_of(fp)] == doctest::Approx(200.0));
  }

  SUBCASE("no loss linking or capacity rows for line")
  {
    CHECK(count_rows_containing(li, "line_flow_link") == 0);
    CHECK(count_rows_containing(li, "line_loss_link") == 0);
    CHECK(count_rows_containing(li, "line_capacity") == 0);
  }
}

// ── linear mode LP structure ───────────────────────────────────────

TEST_CASE("line_losses LP structure - linear mode")
{
  // Use explicit lossfactor=0.05 for predictable coefficients
  LPFixture fix("linear",
                /*loss_segments=*/1,
                /*R=*/0.0,
                /*V=*/0.0,
                /*lossfactor=*/0.05);
  auto& li = fix.lp();

  SUBCASE("two directional flow variables, no loss variables")
  {
    CHECK(count_cols_containing(li, "line_flowp_") == 1);
    CHECK(count_cols_containing(li, "line_flown_") == 1);
    CHECK(count_cols_containing(li, "line_lossp_") == 0);
    CHECK(count_cols_containing(li, "line_seg_") == 0);
  }

  SUBCASE("flow variables have [0, tmax] bounds")
  {
    const auto fp = find_col(li, "line_flowp_");
    const auto fn = find_col(li, "line_flown_");
    CHECK(li.get_col_low()[value_of(fp)] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[value_of(fp)] == doctest::Approx(200.0));
    CHECK(li.get_col_low()[value_of(fn)] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[value_of(fn)] == doctest::Approx(200.0));
  }

  SUBCASE("bus balance coefficients encode loss factor (receiver mode)")
  {
    // Default allocation = receiver:
    //   bus_a (sender):    fp coeff = -1.0,  fn coeff = +(1-λ)
    //   bus_b (receiver):  fp coeff = +(1-λ), fn coeff = -1.0
    const auto fp = find_col(li, "line_flowp_");
    const auto fn = find_col(li, "line_flown_");
    const auto bal_a = find_row(li, "bus_balance_1");
    const auto bal_b = find_row(li, "bus_balance_2");

    // A→B: bus_a sends (-1), bus_b receives (+0.95)
    CHECK(li.get_coeff(bal_a, fp) == doctest::Approx(-1.0));
    CHECK(li.get_coeff(bal_b, fp) == doctest::Approx(0.95));

    // B→A: bus_b sends (-1), bus_a receives (+0.95)
    CHECK(li.get_coeff(bal_b, fn) == doctest::Approx(-1.0));
    CHECK(li.get_coeff(bal_a, fn) == doctest::Approx(0.95));
  }

  SUBCASE("no linking or loss-tracking rows")
  {
    CHECK(count_rows_containing(li, "line_flow_link") == 0);
    CHECK(count_rows_containing(li, "line_loss_link") == 0);
  }
}

// ── piecewise mode LP structure ────────────────────────────────────

TEST_CASE("line_losses LP structure - piecewise mode")
{
  // 3 segments, R=0.01, V=100 → V²=10000
  LPFixture fix("piecewise", /*loss_segments=*/3);
  auto& li = fix.lp();

  SUBCASE("creates fp, fn, loss, and K=3 segment variables")
  {
    CHECK(count_cols_containing(li, "line_flowp_") == 1);
    CHECK(count_cols_containing(li, "line_flown_") == 1);
    CHECK(count_cols_containing(li, "line_lossp_") == 1);
    CHECK(count_cols_containing(li, "line_seg_") == 3);
  }

  SUBCASE("segment variables have [0, fmax/K] bounds")
  {
    // fmax = max(tmax_ab, tmax_ba) = 200, K=3 → width ≈ 66.667
    const double expected_width = 200.0 / 3.0;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_seg_")) {
        CHECK(li.get_col_low()[idx] == doctest::Approx(0.0));
        CHECK(li.get_col_upp()[idx] == doctest::Approx(expected_width));
      }
    }
  }

  SUBCASE("linking row: fp + fn - seg1 - seg2 - seg3 = 0")
  {
    const auto lnk = find_row(li, "line_flow_link_");
    const auto fp = find_col(li, "line_flowp_");
    const auto fn = find_col(li, "line_flown_");

    // Equality constraint: lowb == uppb == 0
    CHECK(li.get_row_low()[value_of(lnk)] == doctest::Approx(0.0));
    CHECK(li.get_row_upp()[value_of(lnk)] == doctest::Approx(0.0));

    CHECK(li.get_coeff(lnk, fp) == doctest::Approx(1.0));
    CHECK(li.get_coeff(lnk, fn) == doctest::Approx(1.0));

    // Each segment has coefficient -1.0 in the linking row
    int seg_count = 0;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_seg_")) {
        CHECK(li.get_coeff(lnk, idx) == doctest::Approx(-1.0));
        ++seg_count;
      }
    }
    CHECK(seg_count == 3);
  }

  SUBCASE("loss-tracking row coefficients match PWL formula")
  {
    // loss_k = width · R · (2k-1) / V²
    // width = 200/3, R = 0.01, V² = 10000
    const double width = 200.0 / 3.0;
    const double R = 0.01;
    const double V2 = 10000.0;

    const auto lsl = find_row(li, "line_loss_link_");
    const auto loss_col = find_col(li, "line_lossp_");

    // Loss variable has coeff +1.0 in loss-tracking row
    CHECK(li.get_coeff(lsl, loss_col) == doctest::Approx(1.0));

    // Collect segment coefficients sorted by name
    std::vector<std::pair<std::string, double>> seg_coeffs;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_seg_")) {
        seg_coeffs.emplace_back(name, li.get_coeff(lsl, idx));
      }
    }
    std::ranges::sort(seg_coeffs);
    REQUIRE(seg_coeffs.size() == 3);

    for (int k = 1; k <= 3; ++k) {
      const double expected = -width * R * ((2.0 * k) - 1.0) / V2;
      CHECK(seg_coeffs[static_cast<size_t>(k - 1)].second
            == doctest::Approx(expected));
    }
  }

  SUBCASE("two rows total for line (linking + loss-tracking)")
  {
    CHECK(count_rows_containing(li, "line_flow_link_") == 1);
    CHECK(count_rows_containing(li, "line_loss_link_") == 1);
    // No per-direction linking rows
    CHECK(count_rows_containing(li, "line_flowp_link") == 0);
    CHECK(count_rows_containing(li, "line_flown_link") == 0);
  }
}

// ── bidirectional mode LP structure ────────────────────────────────

TEST_CASE("line_losses LP structure - bidirectional mode")
{
  // 3 segments per direction, R=0.01, V=100
  LPFixture fix("bidirectional", /*loss_segments=*/3);
  auto& li = fix.lp();

  SUBCASE("creates per-direction flow, loss, and segment variables")
  {
    // flowp_ matches both the base flow var and segment vars (1 + 3 = 4)
    CHECK(count_cols_containing(li, "line_flowp_") == 4);
    CHECK(count_cols_containing(li, "line_flown_") == 4);
    CHECK(count_cols_containing(li, "line_lossp_") == 1);
    CHECK(count_cols_containing(li, "line_lossn_") == 1);
    CHECK(count_cols_containing(li, "line_flowp_seg_") == 3);
    CHECK(count_cols_containing(li, "line_flown_seg_") == 3);
  }

  SUBCASE("per-direction linking and loss-tracking rows (4 total)")
  {
    CHECK(count_rows_containing(li, "line_flowp_link_") == 1);
    CHECK(count_rows_containing(li, "line_flown_link_") == 1);
    CHECK(count_rows_containing(li, "line_lossp_link_") == 1);
    CHECK(count_rows_containing(li, "line_lossn_link_") == 1);
  }

  SUBCASE("positive-direction linking row: fp - seg1 - seg2 - seg3 = 0")
  {
    const auto lnkp = find_row(li, "line_flowp_link_");
    const auto fp = find_col(li, "line_flowp_", "_seg_");

    CHECK(li.get_row_low()[value_of(lnkp)] == doctest::Approx(0.0));
    CHECK(li.get_row_upp()[value_of(lnkp)] == doctest::Approx(0.0));

    CHECK(li.get_coeff(lnkp, fp) == doctest::Approx(1.0));

    int seg_count = 0;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_flowp_seg_")) {
        CHECK(li.get_coeff(lnkp, idx) == doctest::Approx(-1.0));
        ++seg_count;
      }
    }
    CHECK(seg_count == 3);
  }

  SUBCASE("negative-direction loss coefficients match PWL formula")
  {
    const double width = 200.0 / 3.0;
    const double R = 0.01;
    const double V2 = 10000.0;

    const auto lsln = find_row(li, "line_lossn_link_");
    const auto lsn = find_col(li, "line_lossn_");

    CHECK(li.get_coeff(lsln, lsn) == doctest::Approx(1.0));

    std::vector<std::pair<std::string, double>> seg_coeffs;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_flown_seg_")) {
        seg_coeffs.emplace_back(name, li.get_coeff(lsln, idx));
      }
    }
    std::ranges::sort(seg_coeffs);
    REQUIRE(seg_coeffs.size() == 3);

    for (int k = 1; k <= 3; ++k) {
      const double expected = -width * R * ((2.0 * k) - 1.0) / V2;
      CHECK(seg_coeffs[static_cast<size_t>(k - 1)].second
            == doctest::Approx(expected));
    }
  }

  SUBCASE("bidirectional has 2x the rows of piecewise")
  {
    LPFixture fix_pw("piecewise", /*loss_segments=*/3);
    auto& li_pw = fix_pw.lp();

    // bidirectional: flowp_link + flown_link + lossp_link + lossn_link = 4
    const int bidir_line_rows = count_rows_containing(li, "line_flowp_link")
        + count_rows_containing(li, "line_flown_link")
        + count_rows_containing(li, "line_lossp_link")
        + count_rows_containing(li, "line_lossn_link");
    // piecewise: flow_link + loss_link = 2
    const int pw_line_rows = count_rows_containing(li_pw, "line_flow_link")
        + count_rows_containing(li_pw, "line_loss_link");

    CHECK(bidir_line_rows == 4);
    CHECK(pw_line_rows == 2);
  }
}

// ── piecewise_direct mode LP structure ─────────────────────────────

TEST_CASE("line_losses LP structure - piecewise_direct mode")
{
  // 3 segments per direction, R=0.01, V=100 → V²=10000
  LPFixture fix("piecewise_direct", /*loss_segments=*/3);
  auto& li = fix.lp();

  SUBCASE("creates fp_agg, fn_agg + K=3 segments per direction; no loss cols")
  {
    // flowp_ matches aggregation col + 3 segment cols = 4
    CHECK(count_cols_containing(li, "line_flowp_") == 4);
    CHECK(count_cols_containing(li, "line_flown_") == 4);
    CHECK(count_cols_containing(li, "line_flowp_seg_") == 3);
    CHECK(count_cols_containing(li, "line_flown_seg_") == 3);
    // No loss variables — losses are baked into bus-balance coefficients
    CHECK(count_cols_containing(li, "line_lossp_") == 0);
    CHECK(count_cols_containing(li, "line_lossn_") == 0);
  }

  SUBCASE("aggregation col bounds are [0, tmax]")
  {
    const auto fp = find_col(li, "line_flowp_", "_seg_");
    const auto fn = find_col(li, "line_flown_", "_seg_");
    CHECK(li.get_col_low()[value_of(fp)] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[value_of(fp)] == doctest::Approx(200.0));
    CHECK(li.get_col_low()[value_of(fn)] == doctest::Approx(0.0));
    CHECK(li.get_col_upp()[value_of(fn)] == doctest::Approx(200.0));
  }

  SUBCASE("segment bounds are [0, tmax/K]")
  {
    const double expected_width = 200.0 / 3.0;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_flowp_seg_") || name.contains("line_flown_seg_"))
      {
        CHECK(li.get_col_low()[idx] == doctest::Approx(0.0));
        CHECK(li.get_col_upp()[idx] == doctest::Approx(expected_width));
      }
    }
  }

  SUBCASE("two linking rows total (one per direction); no loss-tracking rows")
  {
    CHECK(count_rows_containing(li, "line_flowp_link_") == 1);
    CHECK(count_rows_containing(li, "line_flown_link_") == 1);
    CHECK(count_rows_containing(li, "line_lossp_link_") == 0);
    CHECK(count_rows_containing(li, "line_lossn_link_") == 0);
    CHECK(count_rows_containing(li, "line_loss_link_") == 0);
  }

  SUBCASE("positive linking row: fp_agg − Σ seg_k = 0")
  {
    const auto lnk = find_row(li, "line_flowp_link_");
    const auto fp = find_col(li, "line_flowp_", "_seg_");

    CHECK(li.get_row_low()[value_of(lnk)] == doctest::Approx(0.0));
    CHECK(li.get_row_upp()[value_of(lnk)] == doctest::Approx(0.0));
    CHECK(li.get_coeff(lnk, fp) == doctest::Approx(1.0));

    int seg_count = 0;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_flowp_seg_")) {
        CHECK(li.get_coeff(lnk, idx) == doctest::Approx(-1.0));
        ++seg_count;
      }
    }
    CHECK(seg_count == 3);
  }

  SUBCASE("aggregation col has zero bus-balance coefficient")
  {
    // fp_agg / fn_agg are pure accounting; loss allocation lives on
    // the segment cols.  This preserves the downstream fp_col API for
    // Kirchhoff and reporters without double-counting.
    const auto fp = find_col(li, "line_flowp_", "_seg_");
    const auto fn = find_col(li, "line_flown_", "_seg_");
    const auto bal_a = find_row(li, "bus_balance_1");
    const auto bal_b = find_row(li, "bus_balance_2");

    CHECK(li.get_coeff(bal_a, fp) == doctest::Approx(0.0));
    CHECK(li.get_coeff(bal_b, fp) == doctest::Approx(0.0));
    CHECK(li.get_coeff(bal_a, fn) == doctest::Approx(0.0));
    CHECK(li.get_coeff(bal_b, fn) == doctest::Approx(0.0));
  }

  SUBCASE("positive segments stamp bus balance with per-segment loss factor")
  {
    // PLP `genpdlin.f:143-148`:
    //   sending (bus_a) gets: -1  (receiver allocation)
    //   receiving (bus_b) gets: +(1 - λ_k)
    // where λ_k = width · R · (2k-1) / V².
    const double width = 200.0 / 3.0;
    const double R = 0.01;
    const double V2 = 10000.0;

    const auto bal_a = find_row(li, "bus_balance_1");
    const auto bal_b = find_row(li, "bus_balance_2");

    std::vector<std::pair<std::string, ColIndex>> seg_cols;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_flowp_seg_")) {
        seg_cols.emplace_back(name, idx);
      }
    }
    std::ranges::sort(seg_cols);
    REQUIRE(seg_cols.size() == 3);

    for (int k = 1; k <= 3; ++k) {
      const auto col = seg_cols[static_cast<size_t>(k - 1)].second;
      const double lambda_k = width * R * ((2.0 * k) - 1.0) / V2;
      CHECK(li.get_coeff(bal_a, col) == doctest::Approx(-1.0));
      CHECK(li.get_coeff(bal_b, col)
            == doctest::Approx(1.0 - lambda_k).epsilon(1e-9));
    }
  }

  SUBCASE("negative segments stamp mirrored bus balance")
  {
    // B→A direction: bus_b sends (-1), bus_a receives +(1 - λ_k)
    const double width = 200.0 / 3.0;
    const double R = 0.01;
    const double V2 = 10000.0;

    const auto bal_a = find_row(li, "bus_balance_1");
    const auto bal_b = find_row(li, "bus_balance_2");

    std::vector<std::pair<std::string, ColIndex>> seg_cols;
    for (const auto& [name, idx] : li.col_name_map()) {
      if (name.contains("line_flown_seg_")) {
        seg_cols.emplace_back(name, idx);
      }
    }
    std::ranges::sort(seg_cols);
    REQUIRE(seg_cols.size() == 3);

    for (int k = 1; k <= 3; ++k) {
      const auto col = seg_cols[static_cast<size_t>(k - 1)].second;
      const double lambda_k = width * R * ((2.0 * k) - 1.0) / V2;
      CHECK(li.get_coeff(bal_b, col) == doctest::Approx(-1.0));
      CHECK(li.get_coeff(bal_a, col)
            == doctest::Approx(1.0 - lambda_k).epsilon(1e-9));
    }
  }

  SUBCASE("direct has same row count as piecewise but no loss cols")
  {
    LPFixture fix_pw("piecewise", /*loss_segments=*/3);
    auto& li_pw = fix_pw.lp();

    // piecewise: 1 flow_link + 1 loss_link = 2 line-specific rows
    const int pw_rows = count_rows_containing(li_pw, "line_flow_link")
        + count_rows_containing(li_pw, "line_loss_link");
    // direct: flowp_link + flown_link = 2 line-specific rows
    const int dir_rows = count_rows_containing(li, "line_flowp_link")
        + count_rows_containing(li, "line_flown_link")
        + count_rows_containing(li, "line_loss_link");

    CHECK(pw_rows == 2);
    CHECK(dir_rows == 2);

    // But direct has zero loss variables
    CHECK(count_cols_containing(li, "line_lossp_")
              + count_cols_containing(li, "line_lossn_")
          == 0);
    CHECK(count_cols_containing(li_pw, "line_lossp_")
              + count_cols_containing(li_pw, "line_lossn_")
          == 1);
  }
}

TEST_CASE("line_losses engine - piecewise_direct objective matches piecewise")
{
  const auto obj_dir = solve_with_mode("piecewise_direct");
  const auto obj_pw = solve_with_mode("piecewise");
  const auto obj_bi = solve_with_mode("bidirectional");
  // All three approximate the same quadratic loss for this unidirectional
  // flow case; direct must agree with both to segment granularity.
  CHECK(obj_dir > 1.0);
  CHECK(obj_dir < 1.01);
  CHECK(obj_dir == doctest::Approx(obj_pw).epsilon(0.001));
  CHECK(obj_dir == doctest::Approx(obj_bi).epsilon(0.01));
}

TEST_CASE("line_losses engine - adaptive defaults to piecewise")
{
  // With no explicit mode, global default is adaptive → piecewise on
  // lines without expansion (smallest LP of the PWL modes).  Objective
  // must match explicit piecewise.
  const auto obj_adaptive = solve_with_mode("adaptive");
  const auto obj_piecewise = solve_with_mode("piecewise");
  CHECK(obj_adaptive == doctest::Approx(obj_piecewise).epsilon(1e-9));
}

// ── linear mode auto-compute from R/V ──────────────────────────────

TEST_CASE("line_losses LP structure - linear auto-compute lossfactor from R/V")
{
  // No explicit lossfactor; R=0.01, V=100, fmax=200
  // Auto-computed: λ = R·fmax/V² = 0.01·200/10000 = 0.0002
  LPFixture fix("linear",
                /*loss_segments=*/1,
                /*R=*/0.01,
                /*V=*/100.0,
                /*lossfactor=*/0.0);
  auto& li = fix.lp();

  const auto fp = find_col(li, "line_flowp_");
  const auto bal_b = find_row(li, "bus_balance_2");

  // Receiver mode: bus_b gets +(1-λ) = +0.9998
  CHECK(li.get_coeff(bal_b, fp) == doctest::Approx(1.0 - 0.0002).epsilon(1e-6));
}

// ── dynamic mode structure matches piecewise ───────────────────────

TEST_CASE("line_losses LP structure - dynamic mode matches piecewise")
{
  LPFixture fix_dyn("dynamic", /*loss_segments=*/3);
  LPFixture fix_pw("piecewise", /*loss_segments=*/3);
  auto& li_dyn = fix_dyn.lp();
  auto& li_pw = fix_pw.lp();

  // Same number of columns and rows
  CHECK(li_dyn.get_numcols() == li_pw.get_numcols());
  CHECK(li_dyn.get_numrows() == li_pw.get_numrows());

  // Same variable structure
  CHECK(count_cols_containing(li_dyn, "line_seg_")
        == count_cols_containing(li_pw, "line_seg_"));
  CHECK(count_rows_containing(li_dyn, "line_flow_link_")
        == count_rows_containing(li_pw, "line_flow_link_"));
}

// ── Cross-mode matrix: every LineLossesMode on the same fixture ────

TEST_CASE("line_losses - all modes cross-comparison matrix")
{
  // Single fixture, seven modes, common checks.  Acts as a regression
  // net against any one mode silently breaking its invariants relative
  // to the others.  R=0.01, V=100, fmax=200, 3 segments.
  constexpr int K = 3;
  const double width = 200.0 / K;
  const double R = 0.01;
  const double V2 = 10000.0;

  struct ModeExpect
  {
    std::string_view name;
    int flowp_like_cols;  // cols matching "line_flowp_" (includes segs)
    int flown_like_cols;  // cols matching "line_flown_"
    int seg_cols;  // cols matching "line_seg_" (piecewise/dynamic only)
    int flowp_seg_cols;  // cols matching "line_flowp_seg_"
    int flown_seg_cols;  // cols matching "line_flown_seg_"
    int lossp_cols;  // cols matching "line_lossp_"
    int lossn_cols;  // cols matching "line_lossn_"
    int flow_link_rows;  // rows matching "line_flow_link_"
    int flowp_link_rows;  // rows matching "line_flowp_link_"
    int flown_link_rows;  // rows matching "line_flown_link_"
    int loss_link_rows;  // rows matching "line_loss_link_"
    int lossp_link_rows;
    int lossn_link_rows;
    // Total unique line-loss-engine cols/rows (sum of disjoint matches
    // above; aggregation + segments + loss + all link rows).  Checked
    // against a direct count of cols/rows whose name starts with "line_"
    // minus the capacity-base cols ("line_capainst_", "line_capacost_"),
    // so any hidden col/row added by a mode trips this check.
    int total_loss_cols;
    int total_loss_rows;
  };

  // Totals per mode (per line, per block, no expansion, no Kirchhoff):
  //
  // | mode                 | loss-engine cols | loss-engine rows |
  // |----------------------|-----------------:|-----------------:|
  // | none                 | 1                | 0                |
  // | linear               | 2                | 0                |
  // | piecewise            | K + 3            | 2                |
  // | bidirectional        | 2·(K + 2)        | 4                |
  // | dynamic              | K + 3            | 2                |
  // | adaptive (→piecewise) | K + 3           | 2                |
  // | piecewise_direct     | 2·(K + 1)        | 2                |
  //
  // NOTE: piecewise_direct has MORE cols than piecewise (per-direction
  // segments cannot be shared with direction-dependent loss allocation).
  // Its win is PLP-semantic parity + zero loss cols/rows, not LP size.
  // `adaptive` now picks the smallest-LP PWL model (`piecewise`), so
  // `piecewise_direct` is an opt-in for PLP-diff parity only.
  const std::array<ModeExpect, 7> expect = {{
      {.name = "none",
       .flowp_like_cols = 1,
       .flown_like_cols = 0,
       .seg_cols = 0,
       .flowp_seg_cols = 0,
       .flown_seg_cols = 0,
       .lossp_cols = 0,
       .lossn_cols = 0,
       .flow_link_rows = 0,
       .flowp_link_rows = 0,
       .flown_link_rows = 0,
       .loss_link_rows = 0,
       .lossp_link_rows = 0,
       .lossn_link_rows = 0,
       .total_loss_cols = 1,
       .total_loss_rows = 0},
      {.name = "linear",
       .flowp_like_cols = 1,
       .flown_like_cols = 1,
       .seg_cols = 0,
       .flowp_seg_cols = 0,
       .flown_seg_cols = 0,
       .lossp_cols = 0,
       .lossn_cols = 0,
       .flow_link_rows = 0,
       .flowp_link_rows = 0,
       .flown_link_rows = 0,
       .loss_link_rows = 0,
       .lossp_link_rows = 0,
       .lossn_link_rows = 0,
       .total_loss_cols = 2,
       .total_loss_rows = 0},
      {.name = "piecewise",
       .flowp_like_cols = 1,
       .flown_like_cols = 1,
       .seg_cols = K,
       .flowp_seg_cols = 0,
       .flown_seg_cols = 0,
       .lossp_cols = 1,
       .lossn_cols = 0,
       .flow_link_rows = 1,
       .flowp_link_rows = 0,
       .flown_link_rows = 0,
       .loss_link_rows = 1,
       .lossp_link_rows = 0,
       .lossn_link_rows = 0,
       .total_loss_cols = K + 3,
       .total_loss_rows = 2},
      {.name = "bidirectional",
       .flowp_like_cols = 1 + K,
       .flown_like_cols = 1 + K,
       .seg_cols = 0,
       .flowp_seg_cols = K,
       .flown_seg_cols = K,
       .lossp_cols = 1,
       .lossn_cols = 1,
       .flow_link_rows = 0,
       .flowp_link_rows = 1,
       .flown_link_rows = 1,
       .loss_link_rows = 0,
       .lossp_link_rows = 1,
       .lossn_link_rows = 1,
       .total_loss_cols = 2 * (K + 2),
       .total_loss_rows = 4},
      {.name = "dynamic",  // placeholder → piecewise
       .flowp_like_cols = 1,
       .flown_like_cols = 1,
       .seg_cols = K,
       .flowp_seg_cols = 0,
       .flown_seg_cols = 0,
       .lossp_cols = 1,
       .lossn_cols = 0,
       .flow_link_rows = 1,
       .flowp_link_rows = 0,
       .flown_link_rows = 0,
       .loss_link_rows = 1,
       .lossp_link_rows = 0,
       .lossn_link_rows = 0,
       .total_loss_cols = K + 3,
       .total_loss_rows = 2},
      {.name = "adaptive",  // → piecewise (no expansion)
       .flowp_like_cols = 1,
       .flown_like_cols = 1,
       .seg_cols = K,
       .flowp_seg_cols = 0,
       .flown_seg_cols = 0,
       .lossp_cols = 1,
       .lossn_cols = 0,
       .flow_link_rows = 1,
       .flowp_link_rows = 0,
       .flown_link_rows = 0,
       .loss_link_rows = 1,
       .lossp_link_rows = 0,
       .lossn_link_rows = 0,
       .total_loss_cols = K + 3,
       .total_loss_rows = 2},
      {.name = "piecewise_direct",
       .flowp_like_cols = 1 + K,
       .flown_like_cols = 1 + K,
       .seg_cols = 0,
       .flowp_seg_cols = K,
       .flown_seg_cols = K,
       .lossp_cols = 0,
       .lossn_cols = 0,
       .flow_link_rows = 0,
       .flowp_link_rows = 1,
       .flown_link_rows = 1,
       .loss_link_rows = 0,
       .lossp_link_rows = 0,
       .lossn_link_rows = 0,
       .total_loss_cols = 2 * (K + 1),
       .total_loss_rows = 2},
  }};

  // Count cols/rows whose name starts with `line_` but are NOT part of
  // the CapacityObjectLP base ("line_capainst_", "line_capacost_", and
  // their equality rows of the same names).  This gives a direct count
  // of cols/rows added by the loss engine alone, independent of the
  // per-mode substring categories above — so if any mode adds a hidden
  // col/row that doesn't match one of the known categories, the sum
  // equality fails.
  const auto count_loss_cols = [](const LinearInterface& li) -> int
  {
    int count = 0;
    for (const auto& [name, _idx] : li.col_name_map()) {
      if (name.starts_with("line_") && !name.starts_with("line_capainst_")
          && !name.starts_with("line_capacost_"))
      {
        ++count;
      }
    }
    return count;
  };
  const auto count_loss_rows = [](const LinearInterface& li) -> int
  {
    int count = 0;
    for (const auto& [name, _idx] : li.row_name_map()) {
      if (name.starts_with("line_") && !name.starts_with("line_capainst_")
          && !name.starts_with("line_capacost_"))
      {
        ++count;
      }
    }
    return count;
  };

  // Baseline: solve 'none' once and record totals for delta checks.
  LPFixture fix_none("none", K);
  auto& li_none = fix_none.lp();
  const int none_numcols = li_none.get_numcols();
  const int none_numrows = li_none.get_numrows();
  const int none_loss_cols = count_loss_cols(li_none);
  const int none_loss_rows = count_loss_rows(li_none);
  REQUIRE(none_loss_cols == 1);  // just the bidirectional flow var
  REQUIRE(none_loss_rows == 0);

  for (const auto& e : expect) {
    CAPTURE(e.name);
    LPFixture fix(e.name, K);
    auto& li = fix.lp();

    // ── column counts ──────────────────────────────────────────────
    CHECK(count_cols_containing(li, "line_flowp_") == e.flowp_like_cols);
    CHECK(count_cols_containing(li, "line_flown_") == e.flown_like_cols);
    CHECK(count_cols_containing(li, "line_seg_") == e.seg_cols);
    CHECK(count_cols_containing(li, "line_flowp_seg_") == e.flowp_seg_cols);
    CHECK(count_cols_containing(li, "line_flown_seg_") == e.flown_seg_cols);
    CHECK(count_cols_containing(li, "line_lossp_") == e.lossp_cols);
    CHECK(count_cols_containing(li, "line_lossn_") == e.lossn_cols);

    // ── row counts ─────────────────────────────────────────────────
    CHECK(count_rows_containing(li, "line_flow_link_") == e.flow_link_rows);
    CHECK(count_rows_containing(li, "line_flowp_link_") == e.flowp_link_rows);
    CHECK(count_rows_containing(li, "line_flown_link_") == e.flown_link_rows);
    CHECK(count_rows_containing(li, "line_loss_link_") == e.loss_link_rows);
    CHECK(count_rows_containing(li, "line_lossp_link_") == e.lossp_link_rows);
    CHECK(count_rows_containing(li, "line_lossn_link_") == e.lossn_link_rows);

    // ── total loss-engine cols/rows ────────────────────────────────
    // Hard equality on totals: regression guard against any mode
    // silently growing its LP footprint.  Also validates the expected
    // table above is internally consistent with the per-category counts.
    CHECK(count_loss_cols(li) == e.total_loss_cols);
    CHECK(count_loss_rows(li) == e.total_loss_rows);

    // ── delta vs 'none' baseline ───────────────────────────────────
    // Loss-engine cols/rows are the only per-mode LP changes; everything
    // else (buses, generators, demand, capacity base) is identical.
    CHECK(li.get_numcols() - none_numcols
          == e.total_loss_cols - none_loss_cols);
    CHECK(li.get_numrows() - none_numrows
          == e.total_loss_rows - none_loss_rows);

    // ── solve: every mode must produce a feasible optimum ──────────
    auto result = li.resolve();
    REQUIRE(result.has_value());
    CHECK(result.value() == 0);
    const double obj = li.get_obj_value_raw();
    CHECK(obj > 0.0);

    // ── objective ordering: lossless ≤ lossy ──────────────────────
    if (e.name != "none") {
      // Any lossy mode bills more generation than the lossless case.
      // obj(none) = 100MW · $10 / scale_objective(1000) = 1.0.
      CHECK(obj >= 1.0);
    }

    // ── PWL loss coefficients (where applicable) ──────────────────
    // All PWL modes must use the same per-segment
    //   λ_k = width · R · (2k-1) / V²
    // but encode it differently:
    //  - piecewise / dynamic / adaptive (→piecewise): coefficient -λ_k
    //    on the `loss_link_` row for each `line_seg_` col.
    //  - bidirectional: coefficient -λ_k on `lossn_link_` for each
    //    `line_flown_seg_` col (and analogously for the positive side).
    //  - piecewise_direct: no loss row — +(1 − λ_k) stamped on the
    //    receiver bus-balance row for each `line_flowp_seg_` col.
    if (e.name == "piecewise" || e.name == "dynamic" || e.name == "adaptive") {
      const auto lsl = find_row(li, "line_loss_link_");
      std::vector<std::pair<std::string, double>> coeffs;
      for (const auto& [n, idx] : li.col_name_map()) {
        if (n.contains("line_seg_")) {
          coeffs.emplace_back(n, li.get_coeff(lsl, idx));
        }
      }
      std::ranges::sort(coeffs);
      REQUIRE(coeffs.size() == static_cast<size_t>(K));
      for (int k = 1; k <= K; ++k) {
        const double expected = -width * R * ((2.0 * k) - 1.0) / V2;
        CHECK(coeffs[static_cast<size_t>(k - 1)].second
              == doctest::Approx(expected));
      }
    }

    if (e.name == "bidirectional") {
      const auto lsln = find_row(li, "line_lossn_link_");
      std::vector<std::pair<std::string, double>> coeffs;
      for (const auto& [n, idx] : li.col_name_map()) {
        if (n.contains("line_flown_seg_")) {
          coeffs.emplace_back(n, li.get_coeff(lsln, idx));
        }
      }
      std::ranges::sort(coeffs);
      REQUIRE(coeffs.size() == static_cast<size_t>(K));
      for (int k = 1; k <= K; ++k) {
        const double expected = -width * R * ((2.0 * k) - 1.0) / V2;
        CHECK(coeffs[static_cast<size_t>(k - 1)].second
              == doctest::Approx(expected));
      }
    }

    if (e.name == "piecewise_direct") {
      // Direct: no loss row — λ_k is stamped on the bus-balance row
      // (receiver allocation: bus_b coeff = +(1 - λ_k)).
      const auto bal_b = find_row(li, "bus_balance_2");
      std::vector<std::pair<std::string, double>> coeffs;
      for (const auto& [n, idx] : li.col_name_map()) {
        if (n.contains("line_flowp_seg_")) {
          coeffs.emplace_back(n, li.get_coeff(bal_b, idx));
        }
      }
      std::ranges::sort(coeffs);
      REQUIRE(coeffs.size() == static_cast<size_t>(K));
      for (int k = 1; k <= K; ++k) {
        const double lambda_k = width * R * ((2.0 * k) - 1.0) / V2;
        CHECK(coeffs[static_cast<size_t>(k - 1)].second
              == doctest::Approx(1.0 - lambda_k).epsilon(1e-9));
      }
    }
  }
}

TEST_CASE("line_losses - all modes agree on total-cost ordering")
{
  // One solve per mode; direct, adaptive, piecewise, bidirectional all
  // approximate the same quadratic loss — they must agree.  Linear is
  // a looser single-piece approximation; none is the lower bound.
  const double obj_none = solve_with_mode("none");
  const double obj_linear = solve_with_mode("linear");
  const double obj_piecewise = solve_with_mode("piecewise");
  const double obj_bidirectional = solve_with_mode("bidirectional");
  const double obj_dynamic = solve_with_mode("dynamic");
  const double obj_adaptive = solve_with_mode("adaptive");
  const double obj_direct = solve_with_mode("piecewise_direct");

  // Lossless baseline
  CHECK(obj_none == doctest::Approx(1.0).epsilon(0.001));

  // Every lossy mode costs at least as much as lossless.
  CHECK(obj_linear >= obj_none);
  CHECK(obj_piecewise >= obj_none);
  CHECK(obj_bidirectional >= obj_none);
  CHECK(obj_dynamic >= obj_none);
  CHECK(obj_adaptive >= obj_none);
  CHECK(obj_direct >= obj_none);

  // PWL modes approximate the same quadratic loss → agree closely.
  CHECK(obj_piecewise == doctest::Approx(obj_bidirectional).epsilon(0.01));
  CHECK(obj_piecewise == doctest::Approx(obj_direct).epsilon(0.01));
  // `adaptive` resolves to `piecewise` on no-expansion lines → exact match.
  CHECK(obj_piecewise == doctest::Approx(obj_adaptive).epsilon(1e-9));
  CHECK(obj_piecewise == doctest::Approx(obj_dynamic).epsilon(1e-9));

  // Linear vs PWL: for this fixture (unidirectional, |f|=100, tmax=200)
  // the linear approximation uses λ = R·fmax/V² = 2e-4 while the PWL
  // average at f=100 is slightly different; both are tiny overheads.
  CHECK(obj_linear > 1.0);
  CHECK(obj_linear < 1.01);
}

// ─── IEEE 9-bus losses mode comparison ──────────────────────────────
//
// Solves the IEEE 9-bus system with each LineLossesMode and verifies:
//   (a) all modes solve successfully
//   (b) obj(none) ≤ obj(linear) ≤ obj(piecewise) ≤ obj(bidirectional)
//   (c) losses increase total cost (more generation needed)
//   (d) PWL modes produce higher cost than linear (convex ≥ linear approx)
//
// Resistance values derived from IEEE 9-bus R/X ratios (R ≈ X/10),
// nominal voltage 230 kV.

// clang-format off
constexpr std::string_view ieee9b_losses_json = R"({
  "options": {
    "annual_discount_rate": 0.0,
    "output_format": "csv",
    "output_compression": "uncompressed",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true,
    "loss_segments": 5
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "ieee_9b_losses",
    "bus_array": [
      {"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}, {"uid": 3, "name": "b3"},
      {"uid": 4, "name": "b4"}, {"uid": 5, "name": "b5"}, {"uid": 6, "name": "b6"},
      {"uid": 7, "name": "b7"}, {"uid": 8, "name": "b8"}, {"uid": 9, "name": "b9"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmin": 10, "pmax": 250, "gcost": 20, "capacity": 250},
      {"uid": 2, "name": "g2", "bus": "b2", "pmin": 10, "pmax": 300, "gcost": 35, "capacity": 300},
      {"uid": 3, "name": "g3", "bus": "b3", "pmin":  0, "pmax": 270, "gcost": 30, "capacity": 270}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b5", "lmax": [[125.0]]},
      {"uid": 2, "name": "d2", "bus": "b7", "lmax": [[100.0]]},
      {"uid": 3, "name": "d3", "bus": "b9", "lmax":  [[90.0]]}
    ],
    "line_array": [
      {"uid": 1, "name": "l1_4", "bus_a": "b1", "bus_b": "b4", "reactance": 0.0576, "resistance": 0.006, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 2, "name": "l2_7", "bus_a": "b2", "bus_b": "b7", "reactance": 0.0625, "resistance": 0.006, "voltage": 230, "tmax_ab": 300, "tmax_ba": 300},
      {"uid": 3, "name": "l3_9", "bus_a": "b3", "bus_b": "b9", "reactance": 0.0586, "resistance": 0.006, "voltage": 230, "tmax_ab": 270, "tmax_ba": 270},
      {"uid": 4, "name": "l4_5", "bus_a": "b4", "bus_b": "b5", "reactance": 0.085,  "resistance": 0.009, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 5, "name": "l4_6", "bus_a": "b4", "bus_b": "b6", "reactance": 0.092,  "resistance": 0.009, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 6, "name": "l5_7", "bus_a": "b5", "bus_b": "b7", "reactance": 0.161,  "resistance": 0.016, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 7, "name": "l6_9", "bus_a": "b6", "bus_b": "b9", "reactance": 0.17,   "resistance": 0.017, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 8, "name": "l7_8", "bus_a": "b7", "bus_b": "b8", "reactance": 0.072,  "resistance": 0.007, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250},
      {"uid": 9, "name": "l8_9", "bus_a": "b8", "bus_b": "b9", "reactance": 0.1008, "resistance": 0.010, "voltage": 230, "tmax_ab": 250, "tmax_ba": 250}
    ]
  }
})";
// clang-format on

/// Solve the IEEE 9-bus system with the given line_losses_mode override.
/// Returns (objective_value, solve_status).
auto solve_ieee9b_with_mode(std::string_view mode_name)
    -> std::pair<double, int>
{
  // Parse base planning
  Planning base;
  base.merge(parse_planning_json(ieee9b_losses_json));

  // Override line_losses_mode on every line
  for (auto& line : base.system.line_array) {
    line.line_losses_mode = OptName {std::string(mode_name)};
  }

  PlanningLP planning_lp(std::move(base));
  auto result = planning_lp.resolve();
  if (!result.has_value()) {
    return {0.0, -1};
  }

  auto&& systems = planning_lp.systems();
  const auto& li = systems.front().front().linear_interface();
  return {li.get_obj_value_raw(), result.value()};
}

TEST_CASE("IEEE 9-bus losses modes - all modes solve successfully")
{
  for (const auto* mode : {"none", "linear", "piecewise", "bidirectional"}) {
    CAPTURE(mode);
    auto [obj, status] = solve_ieee9b_with_mode(mode);
    CHECK(status == 1);
    CHECK(obj > 0.0);
  }
}

TEST_CASE("IEEE 9-bus losses modes - objective comparison")
{
  auto [obj_none, st_none] = solve_ieee9b_with_mode("none");
  auto [obj_lin, st_lin] = solve_ieee9b_with_mode("linear");
  auto [obj_pw, st_pw] = solve_ieee9b_with_mode("piecewise");
  auto [obj_bidir, st_bidir] = solve_ieee9b_with_mode("bidirectional");

  REQUIRE(st_none == 1);
  REQUIRE(st_lin == 1);
  REQUIRE(st_pw == 1);
  REQUIRE(st_bidir == 1);

  MESSAGE("obj none:          ", obj_none);
  MESSAGE("obj linear:        ", obj_lin);
  MESSAGE("obj piecewise:     ", obj_pw);
  MESSAGE("obj bidirectional: ", obj_bidir);

  SUBCASE("losses increase cost vs no-loss baseline")
  {
    // With losses, more generation is needed → higher cost
    CHECK(obj_lin > obj_none);
    CHECK(obj_pw > obj_none);
    CHECK(obj_bidir > obj_none);
  }

  SUBCASE("linear and piecewise are close approximations")
  {
    // Both approximate the same quadratic loss function from
    // different angles; their objectives should be close.
    // Linear may slightly over- or under-estimate depending on
    // the operating point vs the linearization point.
    const auto ratio_pw_lin = obj_pw / obj_lin;
    CHECK(ratio_pw_lin == doctest::Approx(1.0).epsilon(0.01));
  }

  SUBCASE("piecewise and bidirectional produce similar objectives")
  {
    // Both are PWL approximations of the same quadratic loss;
    // bidirectional decomposes per direction but the total should
    // be close (within 10% of each other).
    const auto ratio = obj_bidir / obj_pw;
    CHECK(ratio == doctest::Approx(1.0).epsilon(0.10));
  }
}

TEST_CASE("IEEE 9-bus losses modes - dynamic falls back to piecewise")
{
  auto [obj_dyn, st_dyn] = solve_ieee9b_with_mode("dynamic");
  auto [obj_pw, st_pw] = solve_ieee9b_with_mode("piecewise");

  REQUIRE(st_dyn == 1);
  REQUIRE(st_pw == 1);

  // dynamic falls back to piecewise → same objective
  CHECK(obj_dyn == doctest::Approx(obj_pw));
}

TEST_CASE("IEEE 9-bus losses modes - lines have resistance defined")
{
  auto planning = parse_planning_json(ieee9b_losses_json);
  for (const auto& line : planning.system.line_array) {
    CAPTURE(line.name);
    // Resistance should be set (non-null, positive)
    REQUIRE(line.resistance.has_value());
    auto r_val = std::get<double>(line.resistance.value());
    CHECK(r_val > 0.0);

    // Voltage should be set
    REQUIRE(line.voltage.has_value());
    auto v_val = std::get<double>(line.voltage.value());
    CHECK(v_val > 0.0);
  }
}

}  // namespace
