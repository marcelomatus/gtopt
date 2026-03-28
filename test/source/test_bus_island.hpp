// SPDX-License-Identifier: BSD-3-Clause

#include <doctest/doctest.h>
#include <gtopt/bus.hpp>
#include <gtopt/bus_island.hpp>
#include <gtopt/line.hpp>
#include <gtopt/planning_options_lp.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

/// Helper: build a PlanningOptionsLP with Kirchhoff enabled
auto make_kirchhoff_options() -> PlanningOptionsLP
{
  PlanningOptions opts {
      .use_kirchhoff = true,
      .use_single_bus = false,
      .kirchhoff_threshold = 0.0,
  };
  return PlanningOptionsLP {std::move(opts)};
}

/// Helper: build a PlanningOptionsLP with single-bus mode
auto make_single_bus_options() -> PlanningOptionsLP
{
  PlanningOptions opts {
      .use_kirchhoff = true,
      .use_single_bus = true,
  };
  return PlanningOptionsLP {std::move(opts)};
}

/// Helper: build a PlanningOptionsLP with Kirchhoff disabled
auto make_no_kirchhoff_options() -> PlanningOptionsLP
{
  PlanningOptions opts {
      .use_kirchhoff = false,
      .use_single_bus = false,
  };
  return PlanningOptionsLP {std::move(opts)};
}

}  // namespace

// ── DisjointSetUnion unit tests ─────────────────────────────────────

TEST_CASE("DisjointSetUnion basic operations")  // NOLINT
{
  SUBCASE("singleton sets")
  {
    DisjointSetUnion dsu(5);
    CHECK(dsu.size() == 5);
    // Each element is its own root
    for (std::size_t i = 0; i < 5; ++i) {
      CHECK(dsu.find(i) == i);
    }
  }

  SUBCASE("unite merges sets")
  {
    DisjointSetUnion dsu(4);
    CHECK(dsu.unite(0, 1));
    CHECK(dsu.find(0) == dsu.find(1));
    // 2 and 3 are still separate
    CHECK(dsu.find(2) != dsu.find(0));
    CHECK(dsu.find(3) != dsu.find(0));
  }

  SUBCASE("unite returns false for same set")
  {
    DisjointSetUnion dsu(3);
    CHECK(dsu.unite(0, 1));
    CHECK_FALSE(dsu.unite(0, 1));
    CHECK_FALSE(dsu.unite(1, 0));
  }

  SUBCASE("transitive merge")
  {
    DisjointSetUnion dsu(4);
    dsu.unite(0, 1);
    dsu.unite(2, 3);
    dsu.unite(1, 2);
    // All four should be in the same set
    CHECK(dsu.find(0) == dsu.find(3));
  }
}

// ── Island detection tests ──────────────────────────────────────────

TEST_CASE("Island detection - single connected island")  // NOLINT
{
  // 3 buses in a triangle: b0—b1—b2—b0
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
      {
          .uid = 2,
          .name = "b2",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
          .reactance = 0.1,
      },
      {
          .uid = 1,
          .name = "l12",
          .bus_a = SingleId {1},
          .bus_b = SingleId {2},
          .reactance = 0.1,
      },
      {
          .uid = 2,
          .name = "l20",
          .bus_a = SingleId {2},
          .bus_b = SingleId {0},
          .reactance = 0.1,
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  CHECK(num_islands == 1);
  // Exactly one bus should have reference_theta
  int ref_count = 0;
  for (const auto& b : buses) {
    if (b.reference_theta.has_value()) {
      ++ref_count;
    }
  }
  CHECK(ref_count == 1);
  CHECK(buses[0].reference_theta.value_or(-1.0) == doctest::Approx(0.0));
}

TEST_CASE("Island detection - two disconnected islands")  // NOLINT
{
  // Island A: buses 0,1   Island B: buses 2,3
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
      {
          .uid = 2,
          .name = "b2",
          .voltage = 220.0,
      },
      {
          .uid = 3,
          .name = "b3",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
          .reactance = 0.1,
      },
      {
          .uid = 1,
          .name = "l23",
          .bus_a = SingleId {2},
          .bus_b = SingleId {3},
          .reactance = 0.1,
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  CHECK(num_islands == 2);
  // Each island should have exactly one reference bus
  CHECK(buses[0].reference_theta.has_value());
  CHECK(buses[0].reference_theta.value_or(-1.0) == doctest::Approx(0.0));
  CHECK_FALSE(buses[1].reference_theta.has_value());

  CHECK(buses[2].reference_theta.has_value());
  CHECK(buses[2].reference_theta.value_or(-1.0) == doctest::Approx(0.0));
  CHECK_FALSE(buses[3].reference_theta.has_value());
}

TEST_CASE("Island detection - isolated bus (no lines)")  // NOLINT
{
  // Bus 0 connected to bus 1; bus 2 isolated
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
      {
          .uid = 2,
          .name = "b2",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
          .reactance = 0.1,
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  // 2 islands: {b0, b1} and {b2}
  CHECK(num_islands == 2);
  CHECK(buses[0].reference_theta.has_value());
  CHECK(buses[2].reference_theta.has_value());
}

TEST_CASE(
    "Island detection - user-specified reference bus respected")  // NOLINT
{
  // User already set reference_theta on bus 1
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
          .reference_theta = 0.0,
      },
      {
          .uid = 2,
          .name = "b2",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
          .reactance = 0.1,
      },
      {
          .uid = 1,
          .name = "l12",
          .bus_a = SingleId {1},
          .bus_b = SingleId {2},
          .reactance = 0.1,
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  CHECK(num_islands == 1);
  // Only bus 1 should have reference_theta (user-specified)
  CHECK_FALSE(buses[0].reference_theta.has_value());
  CHECK(buses[1].reference_theta.value_or(-1.0) == doctest::Approx(0.0));
  CHECK_FALSE(buses[2].reference_theta.has_value());
}

TEST_CASE("Island detection - single bus mode skipped")  // NOLINT
{
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
          .reactance = 0.1,
      },
  };

  const auto opts = make_single_bus_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  CHECK(num_islands == 0);
  CHECK_FALSE(buses[0].reference_theta.has_value());
  CHECK_FALSE(buses[1].reference_theta.has_value());
}

TEST_CASE("Island detection - kirchhoff disabled skipped")  // NOLINT
{
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
          .reactance = 0.1,
      },
  };

  const auto opts = make_no_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  CHECK(num_islands == 0);
  CHECK_FALSE(buses[0].reference_theta.has_value());
}

TEST_CASE("Island detection - self-loop line ignored")  // NOLINT
{
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
  };
  // Only a self-loop on bus 0 — no real connection
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "loop",
          .bus_a = SingleId {0},
          .bus_b = SingleId {0},
          .reactance = 0.1,
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  // Two islands: each bus is isolated
  CHECK(num_islands == 2);
  CHECK(buses[0].reference_theta.has_value());
  CHECK(buses[1].reference_theta.has_value());
}

TEST_CASE("Island detection - line without reactance ignored")  // NOLINT
{
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "b1",
          .voltage = 220.0,
      },
  };
  // Line has no reactance → not a Kirchhoff line
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l01",
          .bus_a = SingleId {0},
          .bus_b = SingleId {1},
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  // Two isolated buses (line without reactance doesn't connect them)
  CHECK(num_islands == 2);
  CHECK(buses[0].reference_theta.has_value());
  CHECK(buses[1].reference_theta.has_value());
}

TEST_CASE("Island detection - single bus returns 0")  // NOLINT
{
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "b0",
          .voltage = 220.0,
      },
  };
  Array<Line> lines = {};

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  CHECK(num_islands == 0);
}

TEST_CASE("Island detection - name-based SingleId")  // NOLINT
{
  Array<Bus> buses = {
      {
          .uid = 0,
          .name = "alpha",
          .voltage = 220.0,
      },
      {
          .uid = 1,
          .name = "beta",
          .voltage = 220.0,
      },
      {
          .uid = 2,
          .name = "gamma",
          .voltage = 220.0,
      },
  };
  // Line references buses by name
  Array<Line> lines = {
      {
          .uid = 0,
          .name = "l_ab",
          .bus_a = SingleId {Name {"alpha"}},
          .bus_b = SingleId {Name {"beta"}},
          .reactance = 0.1,
      },
  };

  const auto opts = make_kirchhoff_options();
  const auto num_islands =
      detect_islands_and_fix_references(buses, lines, opts);

  // 2 islands: {alpha, beta} and {gamma}
  CHECK(num_islands == 2);
  CHECK(buses[0].reference_theta.has_value());
  CHECK_FALSE(buses[1].reference_theta.has_value());
  CHECK(buses[2].reference_theta.has_value());
}
