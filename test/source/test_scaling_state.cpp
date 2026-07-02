// SPDX-License-Identifier: BSD-3-Clause
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
