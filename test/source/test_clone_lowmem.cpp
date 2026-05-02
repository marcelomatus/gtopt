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

TEST_CASE("clone matrix: low-memory modes × clone routes × kinds")
{
  // Parametric coverage of the meaningful (mode, route, kind) cells:
  //   - {off,      plugin, deep/shallow}  — no snapshot, no compression
  //   - {compress, plugin, deep/shallow}  — snapshot retained, source can
  //   release
  //   - {compress, manual, deep/shallow}  — clone_from_flat path, requires
  //   snapshot
  //
  // Skipped cells:
  //   * {off, manual}: `clone_from_flat` requires a snapshot, but
  //     `set_low_memory(off)` deliberately wipes m_snapshot_
  //     (linear_interface.cpp:249-251) — the combination is by design
  //     incoherent.  Verified separately below as an error case.
  //   * {rebuild, *}: `LowMemoryMode::rebuild` requires a SystemLP-owned
  //     rebuild callback to function — unit-test context (no callback)
  //     short-circuits `release_backend` to a no-op (linear_interface.cpp:
  //     117-122), so the clone behaviour collapses to the `off`-mode path.
  //     Production rebuild semantics are covered by the SDDP integration
  //     tests end-to-end.
  //
  // For each cell:
  //   1. dimensions match the source,
  //   2. clone is independent (bound mutation does not leak to source),
  //   3. write_lp on the clone succeeds and emits the source's labels.
  using Mode = LowMemoryMode;
  using Kind = LinearInterface::CloneKind;

  struct Cell
  {
    const char* mode_tag;
    Mode mode;
    const char* route_tag;
    bool manual;
  };

  for (auto cell : {Cell {"off", Mode::off, "plugin", false},
                    Cell {"compress", Mode::compress, "plugin", false},
                    Cell {"compress", Mode::compress, "manual", true}})
  {
    for (auto kind : {Kind::deep, Kind::shallow}) {
      CAPTURE(cell.mode_tag);
      CAPTURE(cell.route_tag);
      const char* kind_tag = (kind == Kind::deep ? "deep" : "shallow");
      CAPTURE(kind_tag);

      auto [src, flat] = make_lowmem_lp();
      src.set_low_memory(cell.mode);
      // The snapshot is needed by the `manual` route only.  `off` mode
      // deliberately clears it via `set_low_memory(off)` (the code path
      // that originally caused this test's REQUIRE to trip).  We only
      // populate the snapshot when the test cell actually consumes it.
      if (cell.manual) {
        src.save_snapshot(FlatLinearProblem {flat});
        REQUIRE(src.has_snapshot_data());
      }

      auto cloned = cell.manual ? src.clone_from_flat(kind) : src.clone(kind);

      // 1. Dimensions match.
      CHECK(cloned.get_numcols() == src.get_numcols());
      CHECK(cloned.get_numrows() == src.get_numrows());

      // 2. Bound mutation isolation.
      const auto src_low_before = src.get_col_low_raw()[0];
      cloned.set_col_low_raw(ColIndex {0}, 1.5);
      CHECK(src.get_col_low_raw()[0] == doctest::Approx(src_low_before));
      CHECK(cloned.get_col_low_raw()[0] == doctest::Approx(1.5));

      // 3. write_lp on the clone — labels survive both COW paths
      //    and the deep/shallow split.
      const auto stem =
          (std::filesystem::temp_directory_path()
           / std::string {std::string {"test_clone_matrix_"} + cell.mode_tag
                          + "_" + cell.route_tag + "_" + kind_tag})
              .string();
      REQUIRE(cloned.write_lp(stem).has_value());
      const auto content = lowmem_clone_read_file(stem + ".lp");
      CHECK(content.find("gen") != std::string::npos);
      CHECK(content.find("bus") != std::string::npos);

      std::error_code ec;
      std::filesystem::remove(stem + ".lp", ec);
    }
  }
}

TEST_CASE("clone_from_flat throws when source is in off-mode (no snapshot)")
{
  // Documents the (off, manual) cell skipped in the matrix above:
  // `set_low_memory(off)` clears m_snapshot_, so `clone_from_flat`
  // hits its pre-condition guard at linear_interface.cpp:768 and
  // throws "source has no snapshot".  Pinning this contract prevents
  // accidental relaxation that would silently fall through to a
  // load_flat with an empty FlatLinearProblem.
  auto [src, flat] = make_lowmem_lp();
  src.set_low_memory(LowMemoryMode::off);
  REQUIRE_FALSE(src.has_snapshot_data());
  CHECK_THROWS_AS(std::ignore = src.clone_from_flat(), std::runtime_error);
}

TEST_CASE(
    "compress mode: deep clone + source release_backend — clone is "
    "fully independent")
{
  // Sister to the existing shallow-clone-survives test, but for the
  // DEEP path: a deep clone owns its own copy of the metadata at
  // clone time, so the source's compression cycle CANNOT reach the
  // clone's pointers.  Verifies the deep contract holds end-to-end
  // under low-memory pressure, which the shallow-only tests above
  // do not exercise.
  auto [src, flat] = make_lowmem_lp();
  src.set_low_memory(LowMemoryMode::compress);
  src.save_snapshot(FlatLinearProblem {flat});

  auto cloned = src.clone(LinearInterface::CloneKind::deep);
  // Deep clones have use_count = 1 on every wrapped meta member
  // immediately after the clone — no sharing with the source.
  CHECK(src.col_labels_meta_use_count() == 1);
  CHECK(cloned.col_labels_meta_use_count() == 1);

  src.release_backend();
  CHECK(src.is_backend_released());
  // The clone is unaffected — its use counts didn't bump down.
  CHECK(cloned.col_labels_meta_use_count() == 1);

  const auto stem = (std::filesystem::temp_directory_path()
                     / "test_clone_lowmem_deep_release")
                        .string();
  REQUIRE(cloned.write_lp(stem).has_value());
  const auto content = lowmem_clone_read_file(stem + ".lp");
  CHECK(content.find("gen") != std::string::npos);
  CHECK(content.find("bus") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}

TEST_CASE("compress mode: clone_from_flat survives source's release_backend")
{
  // The manual route (clone_from_flat) builds the clone via load_flat
  // from the source's snapshot — bypassing backend.clone() entirely.
  // Once built, the clone holds its own backend + metadata and is
  // independent of the source's lifecycle.  Verify that the source's
  // release_backend (which compresses & clears the source's metadata)
  // does not perturb the clone.
  auto [src, flat] = make_lowmem_lp();
  src.set_low_memory(LowMemoryMode::compress);
  src.save_snapshot(FlatLinearProblem {flat});

  // Snapshot must be uncompressed for clone_from_flat to succeed.
  REQUIRE(src.has_snapshot_data());
  const auto cloned = src.clone_from_flat(LinearInterface::CloneKind::shallow);

  // Trigger the source's compression cycle.
  src.release_backend();
  CHECK(src.is_backend_released());

  // Clone is fully functional after the source's compression.
  CHECK(cloned.get_numcols() == 2);
  CHECK(cloned.get_numrows() == 1);
  CHECK_FALSE(cloned.is_backend_released());

  const auto stem = (std::filesystem::temp_directory_path()
                     / "test_clone_lowmem_manual_release")
                        .string();
  REQUIRE(cloned.write_lp(stem).has_value());
  const auto content = lowmem_clone_read_file(stem + ".lp");
  CHECK(content.find("gen") != std::string::npos);
  CHECK(content.find("bus") != std::string::npos);

  std::error_code ec;
  std::filesystem::remove(stem + ".lp", ec);
}
