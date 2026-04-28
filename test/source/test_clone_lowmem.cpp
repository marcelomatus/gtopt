// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_clone_lowmem.cpp
 * @brief     Verify that low-memory compress/decompress paths play
 *            nicely with shallow clones — i.e. the COW detach in
 *            `compress_labels_meta_if_needed` correctly forks the
 *            metadata so the source's compression doesn't blow away
 *            the clone's view.
 *
 * Scenario: a source LP holds the metadata; a shallow clone shares
 * the metadata via `shared_ptr`; the source is then put into
 * `LowMemoryMode::compress` and `release_backend()`'d.  The compress
 * pass calls `detach_for_write` on every wrapped meta member before
 * clearing it — so the clone's metadata, which now uniquely owns the
 * pre-compress state, must still be usable for `write_lp`.
 *
 * If `compress_labels_meta_if_needed` had been left calling `.clear()`
 * directly on the shared_ptr's underlying vector (instead of going
 * through `detach_for_write`), the clone's metadata would silently
 * disappear and `write_lp` would throw.
 */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/sparse_col.hpp>
#include <gtopt/sparse_row.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace
{

std::string lowmem_clone_read_file(const std::filesystem::path& path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Build a labelled LP via LinearProblem → flatten → load_flat so the
// metadata is populated with class_name / variable_name fields the
// label maker can synthesise from.
std::pair<LinearInterface, FlatLinearProblem> make_lowmem_lp()
{
  LinearProblem lp;
  std::ignore = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 2.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 1,
  });
  std::ignore = lp.add_col({
      .lowb = 0.0,
      .uppb = 10.0,
      .cost = 3.0,
      .class_name = "Gen",
      .variable_name = "p",
      .variable_uid = 2,
  });
  const auto r = lp.add_row({
      .lowb = 5.0,
      .uppb = SparseRow::DblMax,
      .class_name = "Bus",
      .constraint_name = "balance",
      .variable_uid = 1,
  });
  lp.set_coeff(r, ColIndex {0}, 1.0);
  lp.set_coeff(r, ColIndex {1}, 1.0);

  auto flat = lp.flatten(LpMatrixOptions {});
  LinearInterface li;
  li.load_flat(flat);
  li.save_base_numrows();
  return {std::move(li), std::move(flat)};
}

}  // namespace

TEST_CASE(
    "compress_labels_meta_if_needed COW-detaches when a shallow "
    "clone shares the metadata")
{
  auto [src, flat] = make_lowmem_lp();
  src.set_low_memory(LowMemoryMode::compress);
  src.save_snapshot(FlatLinearProblem {flat});

  // Shallow clone BEFORE the compression pass: the clone's
  // `m_col_labels_meta_` etc. share the source's pointers
  // (use_count == 2 on every wrapped member).
  auto cloned = src.clone(LinearInterface::CloneKind::shallow);
  REQUIRE(src.col_labels_meta_use_count() == 2);

  // Trigger the compress pass on the source.  This call goes
  // through `compress_labels_meta_if_needed` which calls
  // `detach_for_write` on every shared meta vector before clearing.
  // The clone must retain the pre-compress metadata.
  src.release_backend();

  // After the detach, source uniquely owns its (now-cleared) copy,
  // and the clone uniquely owns the original (still-populated) one.
  CHECK(src.col_labels_meta_use_count() == 1);
  CHECK(cloned.col_labels_meta_use_count() == 1);

  // The clone can still produce a labelled LP file — proves its
  // metadata wasn't accidentally cleared by the source's compression.
  const auto stem =
      (std::filesystem::temp_directory_path() / "test_clone_lowmem_dump")
          .string();
  REQUIRE(cloned.write_lp(stem).has_value());

  const auto content = lowmem_clone_read_file(stem + ".lp");
  CHECK(content.find("gen") != std::string::npos);
  CHECK(content.find("bus") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}

TEST_CASE(
    "source can reconstruct its backend after the clone retained the "
    "pre-compress metadata")
{
  // Sister test: after the COW detach + compression, the source
  // should still be able to reconstruct its backend (via the
  // snapshot path) and hit the decompress branch in
  // `ensure_labels_meta_decompressed`.  This verifies the
  // detach-then-compress lifecycle restores both sides.
  auto [src, flat] = make_lowmem_lp();
  src.set_low_memory(LowMemoryMode::compress);
  src.save_snapshot(FlatLinearProblem {flat});

  auto cloned = src.clone(LinearInterface::CloneKind::shallow);
  src.release_backend();

  // Both LPs survive — clone via its retained metadata, source via
  // reconstruct.
  src.reconstruct_backend();
  CHECK_FALSE(src.is_backend_released());
  CHECK(src.get_numcols() == 2);
  CHECK(src.get_numrows() == 1);

  // Labels-meta should be back to populated on the source.
  // `write_lp` exercises `generate_labels_from_maps` which calls
  // `ensure_labels_meta_decompressed` — that path includes a
  // `detach_for_write` write back into the source's vector.
  const auto stem =
      (std::filesystem::temp_directory_path() / "test_clone_lowmem_src_dump")
          .string();
  REQUIRE(src.write_lp(stem).has_value());
  const auto content = lowmem_clone_read_file(stem + ".lp");
  CHECK(content.find("gen") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}
