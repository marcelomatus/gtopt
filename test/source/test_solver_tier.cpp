// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_solver_tier.cpp
 * @brief     Unit tests for the Tier-2 SolverTier seam.
 * @copyright BSD-3-Clause
 *
 * SolverTier routes pool-backed LP solves through one interface: a null pool
 * falls back to a direct resolve; a non-null pool submits and awaits.  Both
 * paths must yield the same optimum.
 */
#include <expected>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/sddp_pool.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/solver_tier.hpp>

using namespace gtopt;

// Uniquely-named outer namespace so the anon-namespace helper does not
// collide with other test files under CMake unity builds.
namespace test_solver_tier_ns
{
namespace
{

// max 3 x1 + 2 x2  s.t.  x1 + 2 x2 <= 10,  x1 <= 4,  x2 <= 3
// → x1 = 4, x2 = 3, objective (minimising -3 x1 - 2 x2) = -18.
LinearInterface make_solved_lp(const SolverOptions& opts)
{
  LinearInterface lp;
  const auto x1 = lp.add_col(SparseCol {.uppb = 4.0, .cost = -3.0});
  const auto x2 = lp.add_col(SparseCol {.uppb = 3.0, .cost = -2.0});
  SparseRow row;
  row[x1] = 1.0;
  row[x2] = 2.0;
  row.uppb = 10.0;
  (void)lp.add_row(row);
  auto r = lp.initial_solve(opts);
  REQUIRE(r.has_value());
  REQUIRE(lp.is_optimal());
  return lp;
}

}  // namespace
}  // namespace test_solver_tier_ns

TEST_CASE("SolverTier resolve_via_pool seam")  // NOLINT
{
  using namespace test_solver_tier_ns;
  const SolverOptions opts;

  SUBCASE("null pool falls back to a direct resolve")
  {
    auto lp = make_solved_lp(opts);
    const double obj0 = lp.get_obj_value();
    SolverTier tier {nullptr};
    CHECK(tier.pool() == nullptr);
    auto r = tier.resolve_via_pool(lp, opts, {});
    REQUIRE(r.has_value());
    CHECK(*r == 0);  // optimal
    CHECK(lp.get_obj_value() == doctest::Approx(obj0));
  }

  SUBCASE("pool-backed resolve matches the direct result")
  {
    auto lp = make_solved_lp(opts);
    const double obj0 = lp.get_obj_value();
    auto pool = make_sddp_work_pool();
    SolverTier tier {pool.get()};
    CHECK(tier.pool() == pool.get());
    auto r = tier.resolve_via_pool(lp, opts, {});
    REQUIRE(r.has_value());
    CHECK(*r == 0);
    CHECK(lp.get_obj_value() == doctest::Approx(obj0));
  }

  SUBCASE("set_pool rebinds the tier")
  {
    auto pool = make_sddp_work_pool();
    SolverTier tier {};
    CHECK(tier.pool() == nullptr);
    tier.set_pool(pool.get());
    CHECK(tier.pool() == pool.get());
    tier.set_pool(nullptr);
    CHECK(tier.pool() == nullptr);
  }
}
