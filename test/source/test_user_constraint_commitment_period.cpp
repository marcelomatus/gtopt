/**
 * @file      test_user_constraint_commitment_period.cpp
 * @brief     Regression pin for the per-block stamping behaviour of a
 *            UserConstraint that references ``commitment("X").status``
 *            when ``Commitment.commitment_period`` shares one ``u_G``
 *            across K blocks.
 * @date      2026-05-31
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Context: the commitment-layout MIP review (§A.1 / §A.4) claimed that
 * with a 2-block-per-period grouping, the per-block UC row builder
 * would stamp ``row[u_G] += coef`` once per block and produce a
 * coefficient of ``K · coef = 2`` on each row. That would be a
 * pre-existing P0 bug.
 *
 * Reality (see ``source/user_constraint_lp.cpp:796-827``): the per-block
 * loop constructs a SEPARATE ``SparseRow row`` per block, and
 * ``build_row_from_terms`` walks the expression once per row, so each
 * row gets exactly one stamp of ``coef`` regardless of how many blocks
 * share the same underlying ``ColIndex``. K blocks in a group produce K
 * identical rows (each ``u_G ≥ 1``), not one row with coefficient K.
 *
 * This file pins that behaviour so any future refactor (e.g. introducing
 * an ``add_integer_variable`` choke-point with scope-aware stamping)
 * cannot regress into the K·coef accumulation the reviewer worried about.
 */

#include <doctest/doctest.h>
#include <gtopt/commitment.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/planning_options.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_lp.hpp>
#include <gtopt/user_constraint.hpp>

// SystemLP(system, simulation_lp) uses an empty LpMatrixOptions internally
// even when the planning options carry naming flags — see the note in
// test/source/test_reservoir_discharge_limit.cpp:1529-1538.  This helper
// returns an explicit naming-enabled LpMatrixOptions so col_name_map() /
// row_name_map() are populated.

using namespace gtopt;

namespace ucp
{
namespace
{

/// 1 bus, 1 demand, 1 generator, 1 stage of 4 hourly blocks, 1 scenario.
/// Mirrors the `test_commitment.cpp` builder so the LP layout is familiar.
[[nodiscard]] auto make_basic_case()
{
  struct TC
  {
    System system;
    Simulation simulation;
  };
  TC tc;
  tc.system.name = "uc_commitment_period";
  tc.system.bus_array = {
      {
          .uid = Uid {1},
          .name = "b1",
      },
  };
  tc.system.demand_array = {
      {
          .uid = Uid {1},
          .name = "d1",
          .bus = Uid {1},
          .capacity = 100.0,
      },
  };
  tc.system.generator_array = {
      {
          .uid = Uid {1},
          .name = "g1",
          .bus = Uid {1},
          .pmin = 0.0,
          .pmax = 100.0,
          .gcost = 10.0,
          .capacity = 100.0,
      },
  };
  tc.simulation.block_array = {
      {
          .uid = Uid {0},
          .duration = 1.0,
      },
      {
          .uid = Uid {1},
          .duration = 1.0,
      },
      {
          .uid = Uid {2},
          .duration = 1.0,
      },
      {
          .uid = Uid {3},
          .duration = 1.0,
      },
  };
  tc.simulation.stage_array = {
      {
          .uid = Uid {0},
          .first_block = 0,
          .count_block = 4,
          .chronological = true,
      },
  };
  tc.simulation.scenario_array = {
      {
          .uid = Uid {0},
      },
  };
  return tc;
}

[[nodiscard]] auto make_options()
{
  PlanningOptions popts;
  popts.model_options.demand_fail_cost = 1000.0;
  popts.model_options.use_single_bus = true;
  popts.model_options.scale_objective = 1.0;
  popts.lp_matrix_options.col_with_names = true;
  popts.lp_matrix_options.col_with_name_map = true;
  popts.lp_matrix_options.row_with_names = true;
  popts.lp_matrix_options.row_with_name_map = true;
  return popts;
}

/// Explicit naming-enabled LpMatrixOptions for SystemLP's flat_opts.
[[nodiscard]] auto naming_enabled_flat_opts() -> LpMatrixOptions
{
  LpMatrixOptions flat_opts;
  flat_opts.col_with_names = true;
  flat_opts.row_with_names = true;
  flat_opts.col_with_name_map = true;
  flat_opts.row_with_name_map = true;
  return flat_opts;
}

/// Collect all column indices whose label contains both `class_substr` and
/// `attr_substr`.  Equivalent to `count_cols_containing` in
/// `test_line_losses.cpp` but returning the index set.
[[nodiscard]] auto find_cols(const LinearInterface& li,
                             std::string_view class_substr,
                             std::string_view attr_substr)
    -> std::vector<ColIndex>
{
  std::vector<ColIndex> out;
  for (const auto& [name, idx] : li.col_name_map()) {
    if (name.contains(class_substr) && name.contains(attr_substr)) {
      out.push_back(idx);
    }
  }
  return out;
}

/// Collect all row indices whose label contains `substr`.
[[nodiscard]] auto find_rows(const LinearInterface& li, std::string_view substr)
    -> std::vector<RowIndex>
{
  std::vector<RowIndex> out;
  for (const auto& [name, idx] : li.row_name_map()) {
    if (name.contains(substr)) {
      out.push_back(idx);
    }
  }
  return out;
}

}  // namespace
}  // namespace ucp

TEST_CASE(
    "UC referencing commitment.status under commitment_period: "
    "per-row coefficient is 1.0, not K (regression for §A.1)")
{
  auto tc = ucp::make_basic_case();

  // commitment_period = 2.0h with 4 hourly blocks creates 2 groups:
  // group 0 = {block 0, block 1} sharing ucol_0;
  // group 1 = {block 2, block 3} sharing ucol_1.
  // See `source/commitment_lp.cpp:146-160` for the greedy grouping.
  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 1.0,
          .relax = true,
          .commitment_period = 2.0,
      },
  };

  // Per-block constraint that resolves to commitment.status >= 1 at every
  // block.  No `sum(...)` wrapper — `build_row_from_terms` walks the
  // expression once per block and stamps one term per row.
  tc.system.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "uc_status_pin",
          .expression = Name {R"(commitment("g1_uc").status >= 1)"},
      },
  };

  const auto popts = ucp::make_options();
  const PlanningOptionsLP options(popts);
  SimulationLP sim_lp(tc.simulation, options);
  SystemLP sys_lp(tc.system, sim_lp, ucp::naming_enabled_flat_opts());
  auto&& li = sys_lp.linear_interface();

  SUBCASE("commitment_period produces exactly 2 status cols")
  {
    const auto status_cols = ucp::find_cols(li, "commitment", "status");
    CHECK(status_cols.size() == 2);
  }

  SUBCASE("UC produces exactly 4 constraint rows (one per block)")
  {
    const auto uc_rows = ucp::find_rows(li, "uc_status_pin");
    CHECK(uc_rows.size() == 4);
  }

  SUBCASE("each UC row stamps exactly one status col with coefficient 1.0")
  {
    const auto status_cols = ucp::find_cols(li, "commitment", "status");
    REQUIRE(status_cols.size() == 2);
    const auto uc_rows = ucp::find_rows(li, "uc_status_pin");
    REQUIRE(uc_rows.size() == 4);

    // For each UC row, exactly one of the two status cols should carry a
    // non-zero coefficient, and the coefficient should be exactly 1.0.
    // §A.1 of the MIP review claimed `row[u_G] = K · coef` (= 2 here) —
    // that would mean each row had a coefficient of 2.0.
    for (const auto& row : uc_rows) {
      int nonzero_count = 0;
      double total_coef = 0.0;
      for (const auto& col : status_cols) {
        const auto coef = li.get_coeff(row, col);
        if (coef != 0.0) {
          ++nonzero_count;
          total_coef += coef;
        }
      }
      CHECK(nonzero_count == 1);
      CHECK(total_coef == doctest::Approx(1.0));
    }
  }

  SUBCASE("each status col is referenced by exactly its 2 group blocks")
  {
    // Cross-check from the opposite direction: each ucol receives a stamp
    // from exactly its group's blocks (2 blocks × coefficient 1.0 = 2.0
    // summed across all 4 UC rows).
    const auto status_cols = ucp::find_cols(li, "commitment", "status");
    REQUIRE(status_cols.size() == 2);
    const auto uc_rows = ucp::find_rows(li, "uc_status_pin");
    REQUIRE(uc_rows.size() == 4);

    for (const auto& col : status_cols) {
      double total = 0.0;
      int row_count = 0;
      for (const auto& row : uc_rows) {
        const auto coef = li.get_coeff(row, col);
        if (coef != 0.0) {
          total += coef;
          ++row_count;
        }
      }
      CHECK(row_count == 2);
      CHECK(total == doctest::Approx(2.0));
    }
  }
}

TEST_CASE(
    "UC referencing commitment.status without commitment_period: "
    "identity grouping (1 col per block) — baseline check")
{
  auto tc = ucp::make_basic_case();

  // No commitment_period → one u per block (identity layout, today's default).
  tc.system.commitment_array = {
      {
          .uid = Uid {1},
          .name = "g1_uc",
          .generator = Uid {1},
          .initial_status = 1.0,
          .relax = true,
      },
  };
  tc.system.user_constraint_array = {
      {
          .uid = Uid {1},
          .name = "uc_status_pin",
          .expression = Name {R"(commitment("g1_uc").status >= 1)"},
      },
  };

  const auto popts = ucp::make_options();
  const PlanningOptionsLP options(popts);
  SimulationLP sim_lp(tc.simulation, options);
  SystemLP sys_lp(tc.system, sim_lp, ucp::naming_enabled_flat_opts());
  auto&& li = sys_lp.linear_interface();

  SUBCASE("identity layout: 4 status cols, 4 UC rows, 1:1 mapping")
  {
    const auto status_cols = ucp::find_cols(li, "commitment", "status");
    CHECK(status_cols.size() == 4);
    const auto uc_rows = ucp::find_rows(li, "uc_status_pin");
    CHECK(uc_rows.size() == 4);

    // Every row stamps exactly one status col with coefficient 1.0;
    // every status col is referenced by exactly one UC row.
    for (const auto& row : uc_rows) {
      double total = 0.0;
      int nonzero_count = 0;
      for (const auto& col : status_cols) {
        const auto coef = li.get_coeff(row, col);
        if (coef != 0.0) {
          ++nonzero_count;
          total += coef;
        }
      }
      CHECK(nonzero_count == 1);
      CHECK(total == doctest::Approx(1.0));
    }
  }
}
