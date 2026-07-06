// SPDX-License-Identifier: BSD-3-Clause
#include <array>
#include <span>

#include <doctest/doctest.h>
#include <gtopt/lp_scaling.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// ScalingState was extracted from LinearInterface (step 4, final, of
// decomposing that class, mirroring the MatrixStats / LpLabelStore
// precedents).  The extraction is behaviour-preserving: it must carry the
// COW / clone semantics verbatim —
//   * the Ruiz / equilibration scale vectors, the cost-scale-type vectors
//     and the variable-scale map are `mutable shared_ptr`s (a copy shares
//     storage via atomic incref — the aperture-clone COW invariant);
//   * `equilibration_method` / `obj_constant_raw` are plain per-instance
//     values, value-copied on a clone.
// This test exercises exactly that invariant without standing up a solver.
TEST_CASE("ScalingState value aggregate")  // NOLINT
{
  SUBCASE(
      "default construction: scale ptrs non-null and empty, "
      "equilibration none, obj_constant 0")
  {
    const ScalingState s {};
    // The COW-shared scale / cost-scale-type / variable-scale members
    // default to freshly-allocated empty containers, never null (mirrors
    // the make_shared<> defaults on the pre-extraction members).
    CHECK(s.col_scales != nullptr);
    CHECK(s.row_scales != nullptr);
    CHECK(s.col_cost_scale_types != nullptr);
    CHECK(s.row_cost_scale_types != nullptr);
    CHECK(s.variable_scale_map != nullptr);
    CHECK(s.col_scales->empty());
    CHECK(s.row_scales->empty());
    CHECK(s.col_cost_scale_types->empty());
    CHECK(s.row_cost_scale_types->empty());
    // Plain per-instance scalars default to their documented values.
    CHECK(s.equilibration_method == LpEquilibrationMethod::none);
    CHECK(s.obj_constant_raw == doctest::Approx(0.0));
  }

  SUBCASE("copy shares the shared_ptr scale vectors but value-copies scalars")
  {
    ScalingState original {};
    // Give the shared vectors some content and set the plain scalars.
    original.col_scales->push_back(2.5);
    original.row_scales->push_back(4.0);
    original.col_cost_scale_types->push_back(ConstraintScaleType::Power);
    original.row_cost_scale_types->push_back(ConstraintScaleType::Energy);
    original.equilibration_method = LpEquilibrationMethod::ruiz;
    original.obj_constant_raw = 123.0;

    const ScalingState copy = original;  // clone-equivalent copy

    // COW-shared members: the copy points at the SAME underlying storage.
    // This is the atomic-incref semantics aperture clones rely on.
    CHECK(copy.col_scales.get() == original.col_scales.get());
    CHECK(copy.row_scales.get() == original.row_scales.get());
    CHECK(copy.col_cost_scale_types.get()
          == original.col_cost_scale_types.get());
    CHECK(copy.row_cost_scale_types.get()
          == original.row_cost_scale_types.get());
    CHECK(copy.variable_scale_map.get() == original.variable_scale_map.get());

    // Plain scalars are value-copied.
    CHECK(copy.equilibration_method == LpEquilibrationMethod::ruiz);
    CHECK(copy.obj_constant_raw == doctest::Approx(123.0));

    // Because the scale vectors alias, mutating through the original's
    // shared_ptr is visible through the copy (COW share, not deep copy).
    original.col_scales->push_back(9.0);
    REQUIRE(copy.col_scales->size() == 2);
    CHECK((*copy.col_scales)[ColIndex {1}] == doctest::Approx(9.0));
  }
}

// ScaledView relocated to lp_scaling.hpp alongside ScalingState.  The
// pre-existing direct test (test_linear_interface.cpp) covers only the
// physical-bounds clamp with Op::multiply; these subcases pin the rest
// of the contract in isolation.
TEST_CASE("ScaledView element scaling")  // NOLINT
{
  constexpr std::array data {2.0, 4.0, 8.0};
  constexpr std::array scales {0.5, 2.0, 1.5};

  SUBCASE("multiply applies data[i] * scales[i]")
  {
    const ScaledView v {
        data.data(), 3, scales.data(), scales.size(), ScaledView::Op::multiply};
    REQUIRE(v.size() == 3);
    CHECK(v[0] == doctest::Approx(1.0));
    CHECK(v[1] == doctest::Approx(8.0));
    CHECK(v[2] == doctest::Approx(12.0));
  }

  SUBCASE("divide applies data[i] / scales[i]")
  {
    const ScaledView v {
        data.data(), 3, scales.data(), scales.size(), ScaledView::Op::divide};
    CHECK(v[0] == doctest::Approx(4.0));
    CHECK(v[1] == doctest::Approx(2.0));
    CHECK(v[2] == doctest::Approx(8.0 / 1.5));
  }

  SUBCASE("global factor multiplies every element after per-element scale")
  {
    const ScaledView v {data.data(),
                        3,
                        scales.data(),
                        scales.size(),
                        ScaledView::Op::multiply,
                        10.0};
    CHECK(v[0] == doctest::Approx(10.0));
    CHECK(v[2] == doctest::Approx(120.0));
  }

  SUBCASE("empty scales fall back to raw data (scale 1.0)")
  {
    const ScaledView v {data.data(), 3, nullptr, 0};
    CHECK(v[0] == doctest::Approx(2.0));
    CHECK(v[2] == doctest::Approx(8.0));
  }

  SUBCASE("raw-span constructor is an unscaled pass-through")
  {
    const ScaledView v {std::span<const double> {data}};
    REQUIRE_FALSE(v.empty());
    CHECK(v[1] == doctest::Approx(4.0));
  }

  SUBCASE("iterator visits every scaled element (range-for)")
  {
    const ScaledView v {
        data.data(), 3, scales.data(), scales.size(), ScaledView::Op::multiply};
    double sum = 0.0;
    for (const double x : v) {
      sum += x;
    }
    CHECK(sum == doctest::Approx(1.0 + 8.0 + 12.0));
  }

  SUBCASE("indexing accepts strong index types")
  {
    const ScaledView v {
        data.data(), 3, scales.data(), scales.size(), ScaledView::Op::multiply};
    CHECK(v[ColIndex {1}] == doctest::Approx(8.0));
    CHECK(v[RowIndex {2}] == doctest::Approx(12.0));
  }

  SUBCASE("inverted clamp bounds are ignored (guard, no clamping)")
  {
    // lower > upper in raw space — the clamp guard must skip rather than
    // produce a garbage std::clamp result.
    constexpr std::array lower {5.0, 5.0, 5.0};
    constexpr std::array upper {1.0, 1.0, 1.0};
    const ScaledView v {data.data(),
                        3,
                        scales.data(),
                        scales.size(),
                        lower.data(),
                        3,
                        upper.data(),
                        3,
                        ScaledView::Op::multiply};
    CHECK(v[1] == doctest::Approx(8.0));  // unclamped
  }
}
