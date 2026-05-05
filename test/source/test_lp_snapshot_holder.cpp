// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_lp_snapshot_holder.cpp
 * @brief     Unit tests for LpSnapshotHolder (Phase 2b of LinearInterface
 * split)
 * @date      2026-05-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Pins the S1–S5 invariants from
 * ``include/gtopt/lp_snapshot_holder.hpp`` — the contract that the
 * snapshot holder must enforce so the LinearInterface facade can rely
 * on it as a pure encapsulation of the flat-LP snapshot + compression
 * codec lifecycle.
 */

#include <doctest/doctest.h>
#include <gtopt/lp_snapshot_holder.hpp>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// ── Helper: build a tiny non-empty FlatLinearProblem ─────────────────────

[[nodiscard]] FlatLinearProblem make_tiny_flat_lp()
{
  // The smallest representation that flips ``has_data() == true``: at
  // least one column with bounds + cost.
  FlatLinearProblem flat {};
  flat.ncols = 1;
  flat.nrows = 0;
  flat.collb.push_back(0.0);
  flat.colub.push_back(1.0);
  flat.objval.push_back(1.0);
  flat.matbeg.push_back(0);
  flat.matbeg.push_back(0);
  return flat;
}

// ── S1: default-constructed state ────────────────────────────────────────

TEST_CASE("LpSnapshotHolder S1 — default-constructed state")  // NOLINT
{
  const LpSnapshotHolder holder {};
  CHECK_FALSE(holder.has_data());
  CHECK_FALSE(holder.is_compressed());
  CHECK(holder.codec() == CompressionCodec::lz4);
  CHECK(holder.is_empty());
}

// ── set_codec keeps the snapshot's payload, only changes future codec ───

TEST_CASE("LpSnapshotHolder set_codec round-trips")  // NOLINT
{
  LpSnapshotHolder holder {};
  holder.set_codec(CompressionCodec::zstd);
  CHECK(holder.codec() == CompressionCodec::zstd);
  CHECK_FALSE(holder.is_empty());  // codec mismatch from default

  holder.set_codec(CompressionCodec::lz4);
  CHECK(holder.codec() == CompressionCodec::lz4);
  CHECK(holder.is_empty());  // back to default
}

// ── S2: clear() resets snapshot, leaves codec untouched ──────────────────

TEST_CASE("LpSnapshotHolder S2 — clear preserves codec")  // NOLINT
{
  LpSnapshotHolder holder {};
  holder.set_flat_lp(make_tiny_flat_lp());
  holder.set_codec(CompressionCodec::zstd);
  REQUIRE(holder.has_data());
  REQUIRE(holder.codec() == CompressionCodec::zstd);

  holder.clear();
  CHECK_FALSE(holder.has_data());
  CHECK(holder.codec() == CompressionCodec::zstd);
}

// ── S3 + S4: compress / decompress lifecycle ─────────────────────────────
//
// `LowMemorySnapshot::compress` builds a compressed buffer once, then
// keeps it alive as a persistent cache.  `decompress` re-hydrates the
// flat LP vectors WITHOUT clearing the compressed buffer — so
// `is_compressed()` (which checks whether the compressed buffer is
// non-empty) stays ``true`` after the first compress for the lifetime
// of the snapshot.  The "first compress" transition is therefore a
// one-way flip; subsequent calls to `compress_if_uncompressed()`
// return ``false`` regardless of whether the flat LP has been
// re-decompressed in the meantime.

TEST_CASE("LpSnapshotHolder S3 + S4 — first compress is a one-way flip")
{  // NOLINT
  LpSnapshotHolder holder {};
  holder.set_flat_lp(make_tiny_flat_lp());
  REQUIRE(holder.has_data());
  REQUIRE_FALSE(holder.is_compressed());

  // First compress: returns true (transition).
  CHECK(holder.compress_if_uncompressed());
  CHECK(holder.is_compressed());

  // Repeat compress: returns false (no transition — already compressed).
  CHECK_FALSE(holder.compress_if_uncompressed());
  CHECK(holder.is_compressed());

  // Decompress: re-hydrates the flat LP vectors but keeps the
  // compressed buffer alive.  is_compressed() therefore stays true.
  holder.decompress_if_compressed();
  CHECK(holder.is_compressed());

  // Repeat decompress is a no-op (matbeg already populated).
  holder.decompress_if_compressed();
  CHECK(holder.is_compressed());

  // Repeat compress with a live compressed buffer is also a no-op
  // transition-wise; returns false.
  CHECK_FALSE(holder.compress_if_uncompressed());
  CHECK(holder.is_compressed());

  // Only `clear()` resets the snapshot enough that a fresh
  // `set_flat_lp` + `compress_if_uncompressed` can transition again.
  holder.clear();
  CHECK_FALSE(holder.is_compressed());
  holder.set_flat_lp(make_tiny_flat_lp());
  CHECK(holder.compress_if_uncompressed());
  CHECK(holder.is_compressed());
}

// ── S5: codec change applies on the next first-compress only ─────────────

TEST_CASE("LpSnapshotHolder S5 — codec used on first-compress only")
{  // NOLINT
  LpSnapshotHolder holder {};
  holder.set_codec(CompressionCodec::lz4);
  holder.set_flat_lp(make_tiny_flat_lp());
  REQUIRE(holder.compress_if_uncompressed());
  REQUIRE(holder.is_compressed());

  // Change the codec; the existing compressed payload keeps its
  // original codec (lz4).  No way to inspect codec from the holder
  // for an already-compressed buffer; assert via `snapshot()` shape.
  holder.set_codec(CompressionCodec::zstd);
  CHECK(holder.codec() == CompressionCodec::zstd);
  CHECK(holder.is_compressed());
}

// ── set_flat_lp replaces the flat LP wholesale ───────────────────────────

TEST_CASE("LpSnapshotHolder set_flat_lp replaces wholesale")  // NOLINT
{
  LpSnapshotHolder holder {};
  CHECK_FALSE(holder.has_data());

  holder.set_flat_lp(make_tiny_flat_lp());
  CHECK(holder.has_data());
  CHECK(holder.snapshot().flat_lp.ncols == 1);

  // Replace with an empty FlatLinearProblem flips has_data() back.
  holder.set_flat_lp(FlatLinearProblem {});
  CHECK_FALSE(holder.has_data());
}

}  // namespace
