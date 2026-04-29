// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_cut_tag.cpp
 * @brief     Unit tests for `CutTag`, `sddp_*_tag` constants, and the
 *            `sddp_*_row_prefix` consistency invariants in
 *            `gtopt/sddp_types.hpp`.
 * @date      2026-04-28
 * @copyright BSD-3-Clause
 *
 * The tag constants replaced ~10 sites that previously hand-set
 * `cut.class_name` and `cut.constraint_name` independently.  These
 * tests pin the new invariants:
 *
 *  1. `CutTag::apply_to(row)` stamps both fields atomically and
 *     leaves the rest of the row untouched (lowb/uppb/cmap/scale/
 *     variable_uid/context).
 *  2. Every `sddp_*_tag` carries the constraint-name constant the
 *     name implies (regression guard against a tag being initialised
 *     with the wrong constant).
 *  3. The four `sddp_*_row_prefix` constants exactly match the
 *     runtime label `LabelMaker::make_row_label` produces — the
 *     compile-time `static_assert` already pins the structural
 *     shape, this case asserts the *value* a runtime label
 *     comparison will see.
 *  4. `extract_iteration_from_name` accepts the row prefixes and
 *     rejects unknown ones — the cut-IO dispatcher's contract.
 *
 * Tests use only namespace-scope `constexpr` values, no LP backends,
 * so they run in sub-millisecond and stay independent of any solver
 * plugin.
 */

#include <array>

#include <doctest/doctest.h>
#include <gtopt/iteration.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/lp_context.hpp>
#include <gtopt/lp_matrix_enums.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/sddp_cut_io.hpp>
#include <gtopt/sddp_types.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("CutTag::apply_to stamps class_name and constraint_name atomically")
{
  SparseRow row {.lowb = -1.0, .uppb = 2.0, .scale = 0.5};
  row[ColIndex {3}] = 7.0;
  const CutTag tag {.class_name = "FooClass",
                    .constraint_name = "bar_constraint"};

  auto& ret = tag.apply_to(row);

  CHECK(&ret == &row);  // returns the row reference for chaining
  CHECK(row.class_name == std::string_view {"FooClass"});
  CHECK(row.constraint_name == std::string_view {"bar_constraint"});

  // Other fields untouched
  CHECK(row.lowb == doctest::Approx(-1.0));
  CHECK(row.uppb == doctest::Approx(2.0));
  CHECK(row.scale == doctest::Approx(0.5));
  CHECK(row.cmap.size() == 1);
  CHECK(row.get_coeff(ColIndex {3}) == doctest::Approx(7.0));
}

TEST_CASE("CutTag::apply_to overwrites pre-existing class/constraint names")
{
  SparseRow row {};
  row.class_name = "Stale";
  row.constraint_name = "stale_tag";

  sddp_scut_tag.apply_to(row);

  CHECK(row.class_name == sddp_alpha_class_name);
  CHECK(row.constraint_name == sddp_scut_constraint_name);
}

TEST_CASE("sddp_*_tag constants carry the expected constraint names")
{
  // SDDP-class tags: every one shares `sddp_alpha_class_name`.
  CHECK(sddp_scut_tag.class_name == sddp_alpha_class_name);
  CHECK(sddp_fcut_tag.class_name == sddp_alpha_class_name);
  CHECK(sddp_bcut_tag.class_name == sddp_alpha_class_name);
  CHECK(sddp_ecut_tag.class_name == sddp_alpha_class_name);
  CHECK(sddp_aperture_cut_tag.class_name == sddp_alpha_class_name);
  CHECK(sddp_share_cut_tag.class_name == sddp_alpha_class_name);

  // ...with one constraint name per pass.
  CHECK(sddp_scut_tag.constraint_name == sddp_scut_constraint_name);
  CHECK(sddp_fcut_tag.constraint_name == sddp_fcut_constraint_name);
  CHECK(sddp_bcut_tag.constraint_name == sddp_bcut_constraint_name);
  CHECK(sddp_ecut_tag.constraint_name == sddp_ecut_constraint_name);
  CHECK(sddp_aperture_cut_tag.constraint_name
        == sddp_aperture_cut_constraint_name);
  CHECK(sddp_share_cut_tag.constraint_name == sddp_share_cut_constraint_name);

  // Loader tags share the single `sddp_loaded_cut_constraint_name`
  // but distinct class names.
  CHECK(sddp_loaded_cut_tag.class_name == sddp_loaded_cut_class_name);
  CHECK(sddp_boundary_cut_tag.class_name == sddp_boundary_cut_class_name);
  CHECK(sddp_named_cut_tag.class_name == sddp_named_cut_class_name);
  CHECK(sddp_loaded_cut_tag.constraint_name == sddp_loaded_cut_constraint_name);
  CHECK(sddp_boundary_cut_tag.constraint_name
        == sddp_loaded_cut_constraint_name);
  CHECK(sddp_named_cut_tag.constraint_name == sddp_loaded_cut_constraint_name);
}

TEST_CASE("sddp_<x>_constraint_name constants are pairwise distinct")
{
  // The duplicate detector in `LinearInterface::add_row` uses the
  // (class, constraint) pair as part of the row metadata key, so
  // two passes that share `sddp_alpha_class_name` MUST have
  // different constraint names.
  constexpr std::array constraints {
      sddp_scut_constraint_name,
      sddp_fcut_constraint_name,
      sddp_bcut_constraint_name,
      sddp_ecut_constraint_name,
      sddp_mcut_constraint_name,
      sddp_aperture_cut_constraint_name,
      sddp_share_cut_constraint_name,
  };
  for (std::size_t i = 0; i < constraints.size(); ++i) {
    for (std::size_t j = i + 1; j < constraints.size(); ++j) {
      CAPTURE(i);
      CAPTURE(j);
      CHECK(constraints.at(i) != constraints.at(j));
    }
  }
}

TEST_CASE("sddp_*_row_prefix matches the runtime LabelMaker output")
{
  // Build a representative row for each of the four pass-tagged cut
  // types and verify the labelled name actually starts with the
  // declared prefix.  This catches the (compile-time-checked) prefix
  // definition diverging from the runtime label format.
  const LabelMaker maker {LpNamesLevel::all};

  const auto labelled = [&](const CutTag& tag) -> std::string
  {
    SparseRow row {};
    tag.apply_to(row);
    row.variable_uid = Uid {7};
    row.context = make_iteration_context(make_uid<Scene>(1),
                                         make_uid<Phase>(2),
                                         make_uid<Iteration>(3),
                                         /*extra=*/0);
    return maker.make_row_label(row);
  };

  CHECK(labelled(sddp_scut_tag).starts_with(sddp_scut_row_prefix));
  CHECK(labelled(sddp_fcut_tag).starts_with(sddp_fcut_row_prefix));
  CHECK(labelled(sddp_bcut_tag).starts_with(sddp_bcut_row_prefix));
  CHECK(labelled(sddp_ecut_tag).starts_with(sddp_ecut_row_prefix));
}

TEST_CASE("extract_iteration_from_name accepts every pass-tagged row prefix")
{
  // The dispatcher in `sddp_cut_io.cpp::extract_iteration_from_name`
  // explicitly recognises scut / fcut / bcut (ecut has no iteration
  // field).  Confirm the row-prefix constants align.  The function
  // returns a 0-based `IterationIndex`; we compare its raw integer
  // value (not the 1-based UID `uid_of` would yield).
  // Format: <prefix>{uid}_{scene}_{phase}_{iter}_{offset}
  const auto suffix = std::string {"42_1_2_5_0"};

  CHECK(static_cast<int>(extract_iteration_from_name(
            std::string {sddp_scut_row_prefix} + suffix))
        == 5);
  CHECK(static_cast<int>(extract_iteration_from_name(
            std::string {sddp_fcut_row_prefix} + suffix))
        == 5);
  CHECK(static_cast<int>(extract_iteration_from_name(
            std::string {sddp_bcut_row_prefix} + suffix))
        == 5);

  // Unknown prefix: returns IterationIndex {0}.
  CHECK(static_cast<int>(extract_iteration_from_name("wrong_prefix_42_1_2_5_0"))
        == 0);
}

TEST_CASE("CutTag is a structural-equality aggregate")
{
  // Defensive: ensure the tag stays a trivial aggregate so future
  // edits can't slip in a constructor or virtual that would defeat
  // the constexpr namespace-scope instances.
  static_assert(std::is_aggregate_v<CutTag>);
  static_assert(std::is_trivially_copyable_v<CutTag>);
  static_assert(std::is_standard_layout_v<CutTag>);

  constexpr CutTag a {.class_name = "X", .constraint_name = "y"};
  constexpr CutTag b {.class_name = "X", .constraint_name = "y"};
  constexpr CutTag c {.class_name = "X", .constraint_name = "z"};

  CHECK(a.class_name == b.class_name);
  CHECK(a.constraint_name == b.constraint_name);
  CHECK(a.constraint_name != c.constraint_name);
}
