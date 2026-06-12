// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_clone_use_count.cpp
 * @brief     Direct verification of the `shared_ptr` use_count contract
 *            for `LinearInterface`'s wrapped metadata members.
 *
 * Phase 4 of the shared-ownership refactor wraps eleven members in
 * `std::shared_ptr<T>`.  `clone(shallow)` is a series of atomic
 * incref operations; `clone(deep)` value-copies through
 * `make_shared<T>(*src)`.  Mutating call sites must route through
 * `detach_for_write` so the source's other clones aren't perturbed.
 *
 * These tests catch a class of bugs that the higher-level invariant
 * tests (clone-independence, write_lp byte-equivalence) don't:
 *
 *   1. A missing `detach_for_write` on a mutator — would still pass
 *      single-clone tests because `use_count == 1` makes detach a
 *      no-op, but would silently corrupt other clones at peak.
 *
 *   2. A clone() pathway that forgot to deep-copy a particular
 *      member — would only surface as cross-clone leakage.
 *
 *   3. An accessor returning the wrapped `shared_ptr` instead of
 *      `*shared_ptr` — would leak the lifecycle outside the class
 *      and break `use_count` reasoning.
 *
 * The accessors `col_labels_meta_use_count()` etc. on
 * `LinearInterface` are explicitly intended for tests and
 * diagnostics, exposed in the public header.
 */

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

// Build a minimally-labelled LP for use_count tests.  Production
// metadata must be non-empty so the dedup map and labels vector
// have content to share / detach.
LinearInterface make_use_count_lp()
{
  LinearInterface li;
  std::ignore = li.add_col(SparseCol {
      .uppb = 10.0,
      .cost = 1.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 1,
  });
  SparseRow r {
      .lowb = 0.0,
      .uppb = 100.0,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = 1,
  };
  r[ColIndex {0}] = 1.0;
  std::ignore = li.add_row(r);
  return li;
}

}  // namespace

TEST_CASE("source LP starts with use_count == 1 on every wrapped member")
{
  const auto src = make_use_count_lp();
  CHECK(src.col_labels_meta_use_count() == 1);
  CHECK(src.col_scales_use_count() == 1);
  CHECK(src.row_scales_use_count() == 1);
  CHECK(src.col_meta_index_use_count() == 1);
  CHECK(src.row_meta_index_use_count() == 1);
}

TEST_CASE("clone(shallow) bumps use_count to 2 on every wrapped member")
{
  const auto src = make_use_count_lp();
  const auto cloned = src.clone(LinearInterface::CloneKind::shallow);

  CHECK(src.col_labels_meta_use_count() == 2);
  CHECK(src.col_scales_use_count() == 2);
  CHECK(src.row_scales_use_count() == 2);
  CHECK(src.col_meta_index_use_count() == 2);
  CHECK(src.row_meta_index_use_count() == 2);

  // The clone observes the same use_count.
  CHECK(cloned.col_labels_meta_use_count() == 2);
  CHECK(cloned.col_scales_use_count() == 2);
  CHECK(cloned.row_scales_use_count() == 2);
  CHECK(cloned.col_meta_index_use_count() == 2);
  CHECK(cloned.row_meta_index_use_count() == 2);
}

TEST_CASE("clone(deep) keeps use_count at 1 on every wrapped member")
{
  const auto src = make_use_count_lp();
  const auto cloned = src.clone(LinearInterface::CloneKind::deep);

  CHECK(src.col_labels_meta_use_count() == 1);
  CHECK(src.col_scales_use_count() == 1);
  CHECK(src.row_scales_use_count() == 1);
  CHECK(src.col_meta_index_use_count() == 1);
  CHECK(src.row_meta_index_use_count() == 1);

  CHECK(cloned.col_labels_meta_use_count() == 1);
  CHECK(cloned.col_scales_use_count() == 1);
  CHECK(cloned.row_scales_use_count() == 1);
  CHECK(cloned.col_meta_index_use_count() == 1);
  CHECK(cloned.row_meta_index_use_count() == 1);
}

TEST_CASE("clone destruction restores source's use_count to 1")
{
  const auto src = make_use_count_lp();
  CHECK(src.col_labels_meta_use_count() == 1);
  {
    const auto cloned = src.clone(LinearInterface::CloneKind::shallow);
    CHECK(src.col_labels_meta_use_count() == 2);
  }  // ~LinearInterface fires; cloned releases its shared_ptr ref.
  CHECK(src.col_labels_meta_use_count() == 1);
}

TEST_CASE("multiple shallow clones bump use_count linearly")
{
  const auto src = make_use_count_lp();
  const auto a = src.clone(LinearInterface::CloneKind::shallow);
  const auto b = src.clone(LinearInterface::CloneKind::shallow);
  const auto c = src.clone(LinearInterface::CloneKind::shallow);
  // Source + 3 shallow clones = 4 owners.
  CHECK(src.col_labels_meta_use_count() == 4);
  CHECK(src.col_meta_index_use_count() == 4);
  CHECK(src.col_scales_use_count() == 4);
}

TEST_CASE("set_row_scale on shallow clone detaches m_row_scales_ only")
{
  // The COW detach contract: only the touched member's shared_ptr
  // detaches.  Every other wrapped member stays shared.
  auto src = make_use_count_lp();
  auto cloned = src.clone(LinearInterface::CloneKind::shallow);
  REQUIRE(src.row_scales_use_count() == 2);
  REQUIRE(src.col_scales_use_count() == 2);
  REQUIRE(src.col_labels_meta_use_count() == 2);

  // Mutate row scale on the clone — this calls detach_for_write
  // internally on `m_row_scales_`.
  cloned.set_row_scale(RowIndex {0}, 1.5);

  // The clone's row_scales is now its own (detached).  The source's
  // row_scales is still uniquely owned by source.
  CHECK(src.row_scales_use_count() == 1);
  CHECK(cloned.row_scales_use_count() == 1);
  // Other members stayed shared.
  CHECK(src.col_scales_use_count() == 2);
  CHECK(src.col_labels_meta_use_count() == 2);
  CHECK(src.col_meta_index_use_count() == 2);
  CHECK(src.row_meta_index_use_count() == 2);

  // And the source's row scale is unchanged.
  CHECK(src.get_row_scale(RowIndex {0}) == doctest::Approx(1.0));
  CHECK(cloned.get_row_scale(RowIndex {0}) == doctest::Approx(1.5));
}

TEST_CASE("add_col_disposable on shallow clone leaves shared meta untouched")
{
  // The disposable APIs deliberately go DIRECTLY to the backend +
  // per-clone-local extras; they must NOT detach any shared meta.
  auto src = make_use_count_lp();
  auto cloned = src.clone(LinearInterface::CloneKind::shallow);
  REQUIRE(src.col_labels_meta_use_count() == 2);
  REQUIRE(src.col_meta_index_use_count() == 2);

  std::ignore = cloned.add_col_disposable(SparseCol {
      .uppb = 5.0,
      .class_name = "Disp",
      .variable_name = "x",
      .variable_uid = 7,
  });

  // No member should have detached.
  CHECK(src.col_labels_meta_use_count() == 2);
  CHECK(src.col_meta_index_use_count() == 2);
  CHECK(src.col_scales_use_count() == 2);
  CHECK(src.row_scales_use_count() == 2);
  CHECK(src.row_meta_index_use_count() == 2);
}

TEST_CASE(
    "add_col with non-empty meta on shallow clone leaves the frozen "
    "m_col_labels_meta_ / m_col_meta_index_ shared (post-flatten path)")
{
  // After the metadata-split refactor (b9eaa, follow-up), production
  // `add_col(SparseCol)` writes into the per-instance
  // `m_post_flatten_col_labels_meta_` / `m_post_flatten_col_meta_index_`
  // — NOT the shared frozen `m_col_labels_meta_` / `m_col_meta_index_`.
  // A shallow clone's first post-clone add therefore does NOT detach
  // the frozen vectors: source and clone keep sharing the flatten-side
  // metadata via shared_ptr forever.  The clone's new entry lands in
  // its own per-instance post-flatten vector, invisible to the source.
  //
  // History: this test originally pinned the OPPOSITE invariant
  // ("BOTH must detach…") to document the copy-on-first-add tax that
  // the unified-vector design imposed on aperture clones.  The split
  // eliminates that tax; the test was inverted to lock in the new
  // shared-forever contract.
  auto src = make_use_count_lp();
  auto cloned = src.clone(LinearInterface::CloneKind::shallow);
  REQUIRE(src.col_labels_meta_use_count() == 2);
  REQUIRE(src.col_meta_index_use_count() == 2);

  std::ignore = cloned.add_col(SparseCol {
      .uppb = 3.0,
      .class_name = "Gen",
      .variable_name = "extra",
      .variable_uid = 99,
  });

  // Frozen meta + dedup index stay shared across source and clone.
  CHECK(src.col_labels_meta_use_count() == 2);
  CHECK(src.col_meta_index_use_count() == 2);
  CHECK(cloned.col_labels_meta_use_count() == 2);
  CHECK(cloned.col_meta_index_use_count() == 2);
  // Untouched members still shared.
  CHECK(src.col_scales_use_count() == 2);
  CHECK(src.row_scales_use_count() == 2);
  CHECK(src.row_meta_index_use_count() == 2);

  // Source still has 1 col, clone has 2.
  CHECK(src.get_numcols() == 1);
  CHECK(cloned.get_numcols() == 2);
}
