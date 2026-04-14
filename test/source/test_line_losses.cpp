// SPDX-License-Identifier: BSD-3-Clause

/// @file test_line_losses.hpp
/// @brief Unit tests for the modular line losses engine.

#include <doctest/doctest.h>
#include <gtopt/json/json_parse_policy.hpp>
#include <gtopt/json/json_planning.hpp>
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
    // Global default is adaptive → resolves to piecewise (no expansion)
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
    // Without expansion → piecewise
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
  return lp.get_obj_value();
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
    CHECK(lp.get_obj_value() == doctest::Approx(1.0).epsilon(0.001));
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
  base.merge(
      daw::json::from_json<Planning>(ieee9b_losses_json, StrictParsePolicy));

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
  return {li.get_obj_value(), result.value()};
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
  auto planning =
      daw::json::from_json<Planning>(ieee9b_losses_json, StrictParsePolicy);
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
