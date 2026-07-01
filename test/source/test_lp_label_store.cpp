// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/lp_label_store.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

// LpLabelStore was extracted from LinearInterface (step 3 of decomposing that
// class, mirroring the MatrixStats precedent).  The extraction is
// behaviour-preserving: it must carry two distinct copy semantics verbatim —
//   * the name-to-index maps and the frozen flatten-time label metadata are
//     COW-shared `shared_ptr`s (a copy shares storage via atomic incref);
//   * the post-flatten label vectors are PER-INSTANCE plain value vectors (a
//     copy value-copies them, so each clone owns its own history).
// This test exercises exactly that invariant without standing up a solver.
TEST_CASE("LpLabelStore value aggregate")  // NOLINT
{
  SUBCASE("default construction: maps non-null, post-flatten empty")
  {
    const LpLabelStore s {};
    CHECK(s.name_store == nullptr);
    CHECK(s.spill_key.empty());
    // The COW-shared maps default to freshly-allocated empty containers,
    // never null (mirrors the make_shared<> defaults on the members).
    CHECK(s.row_names != nullptr);
    CHECK(s.col_names != nullptr);
    CHECK(s.col_index_to_name != nullptr);
    CHECK(s.row_index_to_name != nullptr);
    CHECK(s.col_labels_meta != nullptr);
    CHECK(s.row_labels_meta != nullptr);
    CHECK(s.row_names->empty());
    CHECK(s.col_names->empty());
    // Per-instance post-flatten vectors start empty.
    CHECK(s.post_flatten_col_labels_meta.empty());
    CHECK(s.post_flatten_row_labels_meta.empty());
    // Compressed backups / counts / pool default empty.
    CHECK(s.col_labels_meta_count == 0);
    CHECK(s.row_labels_meta_count == 0);
    CHECK(s.label_string_pool.empty());
  }

  SUBCASE("copy shares the shared_ptr maps but not the post-flatten vectors")
  {
    LpLabelStore original {};
    // Give the shared maps + post-flatten vectors some content on the original.
    original.col_names->emplace("gen_cost", ColIndex {3});
    original.row_names->emplace("balance", RowIndex {7});
    original.post_flatten_col_labels_meta.emplace_back();
    original.post_flatten_row_labels_meta.emplace_back();

    const LpLabelStore copy = original;  // clone-equivalent copy

    // COW-shared maps: the copy points at the SAME underlying storage.
    // This is the atomic-incref semantics aperture clones rely on.
    CHECK(copy.col_names.get() == original.col_names.get());
    CHECK(copy.row_names.get() == original.row_names.get());
    CHECK(copy.col_index_to_name.get() == original.col_index_to_name.get());
    CHECK(copy.row_index_to_name.get() == original.row_index_to_name.get());
    CHECK(copy.col_labels_meta.get() == original.col_labels_meta.get());
    CHECK(copy.row_labels_meta.get() == original.row_labels_meta.get());

    // Per-instance post-flatten vectors are INDEPENDENT value copies: they
    // start equal in content but do not alias the original's storage.
    REQUIRE(copy.post_flatten_col_labels_meta.size() == 1);
    REQUIRE(copy.post_flatten_row_labels_meta.size() == 1);

    // Mutating the original's post-flatten vectors must NOT touch the copy —
    // this is the invariant that guarantees each aperture clone owns its own
    // post-flatten history (cuts / slacks / alpha) independently.
    original.post_flatten_col_labels_meta.emplace_back();
    original.post_flatten_col_labels_meta.emplace_back();
    original.post_flatten_row_labels_meta.emplace_back();
    CHECK(original.post_flatten_col_labels_meta.size() == 3);
    CHECK(original.post_flatten_row_labels_meta.size() == 2);
    // The copy is unchanged — no aliasing.
    CHECK(copy.post_flatten_col_labels_meta.size() == 1);
    CHECK(copy.post_flatten_row_labels_meta.size() == 1);
  }
}
