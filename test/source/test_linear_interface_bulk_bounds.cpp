// SPDX-License-Identifier: BSD-3-Clause
//
// Coverage tests for `LinearInterface::set_col_bounds_raw` and
// `LinearInterface::set_col_bounds` — the bulk col-bound counterparts
// of the single-element `set_col_low(_raw)` / `set_col_upp(_raw)`.
//
// Pins the raw/phys contract:
//   1. Raw bulk writes the input verbatim into the backend (after
//      `normalize_bound`), no `col_scale` composition.
//   2. Phys bulk descales by `col_scale[indices[i]]` per element, then
//      routes through the raw path.
//   3. ±DblMax / ±solver infinity / ±std::inf bypass the col_scale
//      divisor entirely (otherwise dividing solver-infinity by a
//      non-unit scale would degrade to a huge-but-finite bound that
//      `normalize_bound` could no longer map back to ±infinity).
//
// These cases mirror the per-element coverage in
// `test_linear_interface_scale.cpp` and the backend-level coverage in
// `test_solver_backend_bulk.cpp`.

#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_replay_buffer.hpp>

using namespace gtopt;
// NOLINTBEGIN(readability-trailing-comma)

namespace
{

// 4-column, 1-row toy LP with a non-trivial col_scale on every column.
// Scales chosen to cover the typical production range:
//   * unit  (1.0)  — no-op descale
//   * >1    (4.0)  — RALCO-style energy scale
//   * <1    (0.5)  — penalty / small-magnitude column
//   * large (10.0) — accentuates rounding for the ±inf-passthrough check
//
// Body row binds the columns into a single LP (`x0+x1+x2+x3 >= 0`) so
// `flatten()` accepts the problem without rejecting it as trivial.
struct ScaledLP4
{
  static constexpr double s0 = 1.0;
  static constexpr double s1 = 4.0;
  static constexpr double s2 = 0.5;
  static constexpr double s3 = 10.0;

  LinearProblem lp {"bulk_bounds_scale"};
  ColIndex c0 {};
  ColIndex c1 {};
  ColIndex c2 {};
  ColIndex c3 {};

  ScaledLP4()
  {
    c0 = lp.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
        .scale = s0,
    });
    c1 = lp.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
        .scale = s1,
    });
    c2 = lp.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
        .scale = s2,
    });
    c3 = lp.add_col(SparseCol {
        .uppb = 100.0,
        .cost = 1.0,
        .scale = s3,
    });

    auto r0 = SparseRow {};
    r0[c0] = 1.0;
    r0[c1] = 1.0;
    r0[c2] = 1.0;
    r0[c3] = 1.0;
    r0.greater_equal(0.0);
    std::ignore = lp.add_row(std::move(r0));
  }

  [[nodiscard]] LinearInterface make_li()
  {
    return LinearInterface {"", lp.flatten({})};
  }
};

}  // namespace

// ────────────────────────────────────────────────────────────────────
// Test 1 — raw bulk writes verbatim, no col_scale composition.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds_raw — writes raw values verbatim, ignoring col_scale")
{
  ScaledLP4 fixture;
  auto li = fixture.make_li();

  REQUIRE(li.get_col_scale(fixture.c0) == doctest::Approx(ScaledLP4::s0));
  REQUIRE(li.get_col_scale(fixture.c1) == doctest::Approx(ScaledLP4::s1));
  REQUIRE(li.get_col_scale(fixture.c2) == doctest::Approx(ScaledLP4::s2));
  REQUIRE(li.get_col_scale(fixture.c3) == doctest::Approx(ScaledLP4::s3));

  // Set both bounds on every column via one bulk dispatch.
  const std::array<ColIndex, 8> idx {
      fixture.c0,
      fixture.c0,
      fixture.c1,
      fixture.c1,
      fixture.c2,
      fixture.c2,
      fixture.c3,
      fixture.c3,
  };
  const std::array<char, 8> lu {
      'L',
      'U',
      'L',
      'U',
      'L',
      'U',
      'L',
      'U',
  };
  const std::array<double, 8> raw {
      1.0,
      11.0,
      2.0,
      12.0,
      3.0,
      13.0,
      4.0,
      14.0,
  };

  li.set_col_bounds_raw(idx, lu, raw);

  const auto lo_raw = li.get_col_low_raw();
  const auto hi_raw = li.get_col_upp_raw();

  // Raw bounds must equal the inputs exactly — no division by col_scale.
  CHECK(lo_raw[fixture.c0] == doctest::Approx(1.0));
  CHECK(hi_raw[fixture.c0] == doctest::Approx(11.0));
  CHECK(lo_raw[fixture.c1] == doctest::Approx(2.0));
  CHECK(hi_raw[fixture.c1] == doctest::Approx(12.0));
  CHECK(lo_raw[fixture.c2] == doctest::Approx(3.0));
  CHECK(hi_raw[fixture.c2] == doctest::Approx(13.0));
  CHECK(lo_raw[fixture.c3] == doctest::Approx(4.0));
  CHECK(hi_raw[fixture.c3] == doctest::Approx(14.0));

  // Physical bounds = raw × col_scale.
  const auto lo_phys = li.get_col_low();
  const auto hi_phys = li.get_col_upp();
  CHECK(lo_phys[fixture.c0] == doctest::Approx(1.0 * ScaledLP4::s0));
  CHECK(hi_phys[fixture.c0] == doctest::Approx(11.0 * ScaledLP4::s0));
  CHECK(lo_phys[fixture.c1] == doctest::Approx(2.0 * ScaledLP4::s1));
  CHECK(hi_phys[fixture.c1] == doctest::Approx(12.0 * ScaledLP4::s1));
  CHECK(lo_phys[fixture.c2] == doctest::Approx(3.0 * ScaledLP4::s2));
  CHECK(hi_phys[fixture.c2] == doctest::Approx(13.0 * ScaledLP4::s2));
  CHECK(lo_phys[fixture.c3] == doctest::Approx(4.0 * ScaledLP4::s3));
  CHECK(hi_phys[fixture.c3] == doctest::Approx(14.0 * ScaledLP4::s3));
}

// ────────────────────────────────────────────────────────────────────
// Test 2 — phys bulk descales by col_scale[idx[i]] per element.
// THIS IS THE HEADLINE CHECK: would catch a missing or wrong descale
// in the phys variant (analog of the production "double-divide" bug
// pinned by test_linear_interface_scale.cpp Test 1).
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds — phys descales by col_scale and routes through raw")
{
  ScaledLP4 fixture;
  auto li = fixture.make_li();

  // Physical bounds on every column: phys / col_scale must end up
  // in the raw vector.
  const std::array<ColIndex, 8> idx {
      fixture.c0,
      fixture.c0,
      fixture.c1,
      fixture.c1,
      fixture.c2,
      fixture.c2,
      fixture.c3,
      fixture.c3,
  };
  const std::array<char, 8> lu {
      'L',
      'U',
      'L',
      'U',
      'L',
      'U',
      'L',
      'U',
  };
  const std::array<double, 8> phys {
      0.0,
      40.0,  // c0: scale 1.0
      0.0,
      40.0,  // c1: scale 4.0  → raw 0, 10
      0.0,
      40.0,  // c2: scale 0.5  → raw 0, 80
      0.0,
      40.0,  // c3: scale 10.0 → raw 0, 4
  };

  li.set_col_bounds(idx, lu, phys);

  const auto lo_raw = li.get_col_low_raw();
  const auto hi_raw = li.get_col_upp_raw();

  // c0 (scale 1.0)
  CHECK(lo_raw[fixture.c0] == doctest::Approx(0.0));
  CHECK(hi_raw[fixture.c0] == doctest::Approx(40.0 / ScaledLP4::s0));
  // c1 (scale 4.0)
  CHECK(lo_raw[fixture.c1] == doctest::Approx(0.0));
  CHECK(hi_raw[fixture.c1] == doctest::Approx(40.0 / ScaledLP4::s1));
  // c2 (scale 0.5)
  CHECK(lo_raw[fixture.c2] == doctest::Approx(0.0));
  CHECK(hi_raw[fixture.c2] == doctest::Approx(40.0 / ScaledLP4::s2));
  // c3 (scale 10.0)
  CHECK(lo_raw[fixture.c3] == doctest::Approx(0.0));
  CHECK(hi_raw[fixture.c3] == doctest::Approx(40.0 / ScaledLP4::s3));

  // Round-trip: physical getters must recover the original inputs.
  const auto lo_phys = li.get_col_low();
  const auto hi_phys = li.get_col_upp();
  CHECK(lo_phys[fixture.c0] == doctest::Approx(0.0));
  CHECK(hi_phys[fixture.c0] == doctest::Approx(40.0));
  CHECK(hi_phys[fixture.c1] == doctest::Approx(40.0));
  CHECK(hi_phys[fixture.c2] == doctest::Approx(40.0));
  CHECK(hi_phys[fixture.c3] == doctest::Approx(40.0));
}

// ────────────────────────────────────────────────────────────────────
// Test 3 — phys bulk passes ±DblMax / ±solver infinity / ±std::inf
// through WITHOUT applying col_scale.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds — ±inf-like values bypass col_scale (no double-scale)")
{
  ScaledLP4 fixture;
  auto li = fixture.make_li();

  const double solver_inf = li.infinity();
  const double std_inf = std::numeric_limits<double>::infinity();
  REQUIRE(solver_inf > 0.0);
  // Finite-sentinel backends (COIN DBL_MAX-style) have std_inf strictly
  // above solver_inf; true-IEEE-infinity backends (HiGHS) have them equal
  // — the flavors below then collapse, which is fine: the passthrough
  // contract is checked via is_pos_inf / is_neg_inf either way.
  REQUIRE(std_inf >= solver_inf);

  // Mix every infinity flavor across columns with different scales.
  // c0 (scale 1.0)   : lower = -DblMax,      upper = +DblMax
  // c1 (scale 4.0)   : lower = -solver_inf,  upper = +solver_inf
  // c2 (scale 0.5)   : lower = -std::inf,    upper = +std::inf
  // c3 (scale 10.0)  : lower = -DblMax,      upper = +std::inf
  const std::array<ColIndex, 8> idx {
      fixture.c0,
      fixture.c0,
      fixture.c1,
      fixture.c1,
      fixture.c2,
      fixture.c2,
      fixture.c3,
      fixture.c3,
  };
  const std::array<char, 8> lu {
      'L',
      'U',
      'L',
      'U',
      'L',
      'U',
      'L',
      'U',
  };
  const std::array<double, 8> phys {
      -LinearProblem::DblMax,
      +LinearProblem::DblMax,
      -solver_inf,
      +solver_inf,
      -std_inf,
      +std_inf,
      -LinearProblem::DblMax,
      +std_inf,
  };

  li.set_col_bounds(idx, lu, phys);

  const auto lo_raw = li.get_col_low_raw();
  const auto hi_raw = li.get_col_upp_raw();

  // Every lower must be normalised to -solver_inf, every upper to
  // +solver_inf — regardless of col_scale.  Use the public helpers
  // `is_pos_inf` / `is_neg_inf` for robustness across solvers.
  CHECK(li.is_neg_inf(lo_raw[fixture.c0]));
  CHECK(li.is_pos_inf(hi_raw[fixture.c0]));
  CHECK(li.is_neg_inf(lo_raw[fixture.c1]));
  CHECK(li.is_pos_inf(hi_raw[fixture.c1]));
  CHECK(li.is_neg_inf(lo_raw[fixture.c2]));
  CHECK(li.is_pos_inf(hi_raw[fixture.c2]));
  CHECK(li.is_neg_inf(lo_raw[fixture.c3]));
  CHECK(li.is_pos_inf(hi_raw[fixture.c3]));

  // Bug-detection: had the phys setter divided solver_inf by the
  // non-unit scale on c1 (= 4.0), the raw upper would be
  // solver_inf / 4 — well below the solver's infinity threshold —
  // and `is_pos_inf` would return false.  The check above pins the
  // ±inf-passthrough contract.
}

// ────────────────────────────────────────────────────────────────────
// Test 4 — 'B' (both) sets lower and upper to the same value, with
// col_scale descale applied to both sides at once.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("set_col_bounds — 'B' pins lower == upper with descale")  // NOLINT
{
  ScaledLP4 fixture;
  auto li = fixture.make_li();

  const std::array<ColIndex, 2> idx {
      fixture.c1,
      fixture.c3,
  };
  const std::array<char, 2> lu {
      'B',
      'B',
  };
  const std::array<double, 2> phys {
      20.0,
      50.0,
  };

  li.set_col_bounds(idx, lu, phys);

  const auto lo_raw = li.get_col_low_raw();
  const auto hi_raw = li.get_col_upp_raw();

  // c1: phys 20 / scale 4 = raw 5
  CHECK(lo_raw[fixture.c1] == doctest::Approx(20.0 / ScaledLP4::s1));
  CHECK(hi_raw[fixture.c1] == doctest::Approx(20.0 / ScaledLP4::s1));
  // c3: phys 50 / scale 10 = raw 5
  CHECK(lo_raw[fixture.c3] == doctest::Approx(50.0 / ScaledLP4::s3));
  CHECK(hi_raw[fixture.c3] == doctest::Approx(50.0 / ScaledLP4::s3));

  // Phys round-trip.
  CHECK(li.get_col_low()[fixture.c1] == doctest::Approx(20.0));
  CHECK(li.get_col_upp()[fixture.c1] == doctest::Approx(20.0));
  CHECK(li.get_col_low()[fixture.c3] == doctest::Approx(50.0));
  CHECK(li.get_col_upp()[fixture.c3] == doctest::Approx(50.0));
}

// ────────────────────────────────────────────────────────────────────
// Test 5 — raw bulk parity with per-element set_col_low_raw /
// set_col_upp_raw at the LinearInterface boundary.  Pins the contract
// that the bulk path produces the same observable state as the
// per-element path (modulo a single backend dispatch).
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds_raw — parity with per-element raw setters at "
    "LinearInterface boundary")
{
  ScaledLP4 fixture_a;
  ScaledLP4 fixture_b;
  auto a = fixture_a.make_li();
  auto b = fixture_b.make_li();

  // Per-element path.
  a.set_col_low_raw(fixture_a.c0, 1.5);
  a.set_col_upp_raw(fixture_a.c0, 9.5);
  a.set_col_low_raw(fixture_a.c2, 2.5);
  a.set_col_upp_raw(fixture_a.c2, 8.5);

  // Bulk path with the same operations.
  const std::array<ColIndex, 4> idx {
      fixture_b.c0,
      fixture_b.c0,
      fixture_b.c2,
      fixture_b.c2,
  };
  const std::array<char, 4> lu {
      'L',
      'U',
      'L',
      'U',
  };
  const std::array<double, 4> vals {
      1.5,
      9.5,
      2.5,
      8.5,
  };
  b.set_col_bounds_raw(idx, lu, vals);

  const auto a_lo = a.get_col_low_raw();
  const auto a_hi = a.get_col_upp_raw();
  const auto b_lo = b.get_col_low_raw();
  const auto b_hi = b.get_col_upp_raw();
  REQUIRE(a_lo.size() == b_lo.size());
  for (std::size_t i = 0; i < a_lo.size(); ++i) {
    CAPTURE(i);
    CHECK(a_lo[i] == doctest::Approx(b_lo[i]));
    CHECK(a_hi[i] == doctest::Approx(b_hi[i]));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 6 — edge cases: empty input, span size mismatch.
// ────────────────────────────────────────────────────────────────────
TEST_CASE("set_col_bounds_raw / set_col_bounds — edge cases")  // NOLINT
{
  ScaledLP4 fixture;
  auto li = fixture.make_li();

  SUBCASE("empty input is a no-op")
  {
    const auto lo_before = li.get_col_low_raw();
    const auto hi_before = li.get_col_upp_raw();
    const std::vector<double> snap_lo {lo_before.begin(), lo_before.end()};
    const std::vector<double> snap_hi {hi_before.begin(), hi_before.end()};

    li.set_col_bounds_raw({}, {}, {});
    li.set_col_bounds({}, {}, {});

    const auto lo_after = li.get_col_low_raw();
    const auto hi_after = li.get_col_upp_raw();
    REQUIRE(lo_after.size() == snap_lo.size());
    for (std::size_t i = 0; i < snap_lo.size(); ++i) {
      CAPTURE(i);
      CHECK(lo_after[i] == doctest::Approx(snap_lo[i]));
      CHECK(hi_after[i] == doctest::Approx(snap_hi[i]));
    }
  }

  SUBCASE("span size mismatch throws std::invalid_argument")
  {
    const std::array<ColIndex, 2> idx {
        fixture.c0,
        fixture.c1,
    };
    const std::array<char, 1> lu_short {
        'L',
    };
    const std::array<double, 2> vals {
        1.0,
        2.0,
    };
    CHECK_THROWS_AS(li.set_col_bounds_raw(idx, lu_short, vals),
                    std::invalid_argument);
    CHECK_THROWS_AS(li.set_col_bounds(idx, lu_short, vals),
                    std::invalid_argument);
  }

  SUBCASE("invalid lu char is silently skipped (matches backend fallback)")
  {
    // 'X' is neither L/U/B → no-op for that entry; valid entries on
    // either side still apply.
    const std::array<ColIndex, 3> idx {
        fixture.c0,
        fixture.c1,
        fixture.c2,
    };
    const std::array<char, 3> lu {
        'L',
        'X',
        'U',
    };
    const std::array<double, 3> vals {
        7.0,
        999.0,
        11.0,
    };
    li.set_col_bounds_raw(idx, lu, vals);

    const auto lo_raw = li.get_col_low_raw();
    const auto hi_raw = li.get_col_upp_raw();
    CHECK(lo_raw[fixture.c0] == doctest::Approx(7.0));
    CHECK(hi_raw[fixture.c2] == doctest::Approx(11.0));
    // c1 received an 'X' entry — neither bound should match 999.
    CHECK(lo_raw[fixture.c1] != doctest::Approx(999.0));
    CHECK(hi_raw[fixture.c1] != doctest::Approx(999.0));
  }
}

// ────────────────────────────────────────────────────────────────────
// Test 10 — bulk-set bounds capture into the replay buffer.
//
// Regression guard for the unification: per-element setters used to
// each record into the replay buffer individually.  After routing them
// through `set_col_bounds_raw`, the bulk path must still record each
// element so a `release_backend → reconstruct_backend` cycle replays
// every updated bound (matches the contract previously verified by
// `LinearInterface — low_memory reconstruct with dynamic cols`).
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds_raw — every element is recorded in the replay buffer")
{
  ScaledLP4 fixture;
  auto li = fixture.make_li();

  // Activate the replay-buffer tracking path (off by default for raw
  // user-space writes; compress mode is the production trigger).
  li.set_low_memory(LowMemoryMode::compress);

  const std::array<ColIndex, 4> idx {
      fixture.c0,
      fixture.c1,
      fixture.c2,
      fixture.c3,
  };
  const std::array<char, 4> lu {
      'L',
      'U',
      'B',
      'L',
  };
  const std::array<double, 4> vals {
      2.5,
      50.0,
      9.0,
      -7.5,
  };
  li.set_col_bounds_raw(idx, lu, vals);

  // Every distinct column index should have a pending entry.
  const auto& pending = li.replay_buf().pending_col_bounds();
  CHECK(pending.contains(fixture.c0));
  CHECK(pending.contains(fixture.c1));
  CHECK(pending.contains(fixture.c2));
  CHECK(pending.contains(fixture.c3));
  CHECK(pending.at(fixture.c0).first == doctest::Approx(2.5));  // L only
  CHECK(pending.at(fixture.c1).second == doctest::Approx(50.0));  // U only
  // 'B' writes both sides on c2.
  CHECK(pending.at(fixture.c2).first == doctest::Approx(9.0));
  CHECK(pending.at(fixture.c2).second == doctest::Approx(9.0));
  CHECK(pending.at(fixture.c3).first == doctest::Approx(-7.5));  // L only
}

// ────────────────────────────────────────────────────────────────────
// Test 11 — bulk-set bounds (incl. ±DblMax) survive a release+reconstruct
// cycle under LowMemoryMode::compress.
//
// This is the end-to-end version of Test 10: the buffer recording is
// only useful if `apply_post_load_replay` (which now also routes
// through `set_col_bounds_raw`) actually re-applies the values on
// reconstruct.  Includes a `±DblMax` entry to pin the ±∞ contract
// across the round-trip.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds_raw — finite + ±DblMax bounds survive release+reconstruct"
    " under compress mode")
{
  ScaledLP4 fixture;
  auto flat = fixture.lp.flatten({});

  LinearInterface li;
  li.set_low_memory(LowMemoryMode::compress);
  li.load_flat(flat);
  li.save_snapshot(flat);
  li.save_base_numrows();
  REQUIRE(li.has_snapshot_data());

  // Bulk-bound update: finite values on c0/c2, +DblMax on c1, -DblMax on c3.
  const std::array<ColIndex, 4> idx {
      fixture.c0,
      fixture.c1,
      fixture.c2,
      fixture.c3,
  };
  const std::array<char, 4> lu {
      'U',
      'U',
      'L',
      'L',
  };
  const std::array<double, 4> vals {
      42.0,
      LinearProblem::DblMax,
      -3.0,
      -LinearProblem::DblMax,
  };
  li.set_col_bounds_raw(idx, lu, vals);

  // Pin pre-cycle state.
  const auto inf_pre = li.infinity();
  const auto hi_pre = li.get_col_upp_raw();
  const auto lo_pre = li.get_col_low_raw();
  CHECK(hi_pre[fixture.c0] == doctest::Approx(42.0));
  CHECK(hi_pre[fixture.c1] >= inf_pre * 0.999);  // normalised to +inf
  CHECK(lo_pre[fixture.c2] == doctest::Approx(-3.0));
  CHECK(lo_pre[fixture.c3] <= -inf_pre * 0.999);  // normalised to -inf

  // Round-trip through release/reconstruct — `apply_post_load_replay`
  // now routes through the bulk raw API; the same bounds should
  // re-appear on the rebuilt backend.
  li.release_backend();
  li.reconstruct_backend();

  const auto inf_post = li.infinity();
  const auto hi_post = li.get_col_upp_raw();
  const auto lo_post = li.get_col_low_raw();
  CHECK(hi_post[fixture.c0] == doctest::Approx(42.0));
  CHECK(hi_post[fixture.c1] >= inf_post * 0.999);
  CHECK(lo_post[fixture.c2] == doctest::Approx(-3.0));
  CHECK(lo_post[fixture.c3] <= -inf_post * 0.999);
}

// ────────────────────────────────────────────────────────────────────
// Test 12 — phys bulk-set bounds (incl. ±solver_inf) survive a clone
// via `clone_from_flat(with_replay=true)`.
//
// The aperture-clone path (PR comment context) uses
// `clone_from_flat(with_replay=true)` so the shallow clone inherits
// every pending bound override.  This test pins that:
//   (a) the phys setter records ±solver_inf passthrough into the
//       replay buffer (no col_scale divide),
//   (b) the clone sees the same effective bounds after replay.
// ────────────────────────────────────────────────────────────────────
TEST_CASE(  // NOLINT
    "set_col_bounds (phys) — ±solver_inf passthrough survives clone_from_flat")
{
  ScaledLP4 fixture;
  auto flat = fixture.lp.flatten({});

  LinearInterface li;
  li.set_low_memory(LowMemoryMode::compress);
  li.load_flat(flat);
  li.save_snapshot(flat);
  li.save_base_numrows();
  REQUIRE(li.has_snapshot_data());

  const double solver_inf = li.infinity();

  // Phys bulk-set: finite phys value on c0 (s0=1.0; descale-noop),
  // finite on c1 (s1=4.0; descales to 25.0 raw), +solver_inf on c2
  // (must NOT descale — would yield finite-but-huge), -solver_inf
  // on c3.
  const std::array<ColIndex, 4> idx {
      fixture.c0,
      fixture.c1,
      fixture.c2,
      fixture.c3,
  };
  const std::array<char, 4> lu {
      'U',
      'U',
      'U',
      'L',
  };
  const std::array<double, 4> phys_vals {
      8.0,
      100.0,
      solver_inf,
      -solver_inf,
  };
  li.set_col_bounds(idx, lu, phys_vals);

  // Source-side checks: phys descale on c1, passthrough on c2/c3.
  const auto hi_src = li.get_col_upp_raw();
  const auto lo_src = li.get_col_low_raw();
  CHECK(hi_src[fixture.c0] == doctest::Approx(8.0));  // s0=1
  CHECK(hi_src[fixture.c1] == doctest::Approx(25.0));  // 100/4
  CHECK(hi_src[fixture.c2] >= solver_inf * 0.999);  // passthrough
  CHECK(lo_src[fixture.c3] <= -solver_inf * 0.999);  // passthrough

  // Clone via `clone_from_flat(with_replay=true)` — the pending
  // bound overrides must be re-applied on the clone after it loads
  // the same flat snapshot.  This is the exact path used by SDDP
  // aperture clones in the production hot loop.
  const auto clone = li.clone_from_flat(LinearInterface::CloneKind::shallow);

  const double clone_inf = clone.infinity();
  const auto hi_clone = clone.get_col_upp_raw();
  const auto lo_clone = clone.get_col_low_raw();
  CHECK(hi_clone[fixture.c0] == doctest::Approx(8.0));
  CHECK(hi_clone[fixture.c1] == doctest::Approx(25.0));
  CHECK(hi_clone[fixture.c2] >= clone_inf * 0.999);
  CHECK(lo_clone[fixture.c3] <= -clone_inf * 0.999);
}

// NOLINTEND(readability-trailing-comma)
