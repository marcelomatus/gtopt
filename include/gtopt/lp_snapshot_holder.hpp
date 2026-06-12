/**
 * @file      lp_snapshot_holder.hpp
 * @brief     LpSnapshotHolder — snapshot + codec + compression facade
 * @date      2026-05-04
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Phase 2b of the LinearInterface split (B2) — see
 * ``docs/linear_interface_split_plan.md`` §2.2 for the broader plan.
 *
 * `LpSnapshotHolder` owns the 2 fields that previously lived directly
 * on `LinearInterface`:
 *
 *   * `m_snapshot_`     — `LowMemorySnapshot` carrying the flat LP
 *                         (uncompressed or compressed buffer) used by
 *                         the `compress`/`snapshot` reconstruct path.
 *   * `m_memory_codec_` — `CompressionCodec` selected at
 *                         `set_low_memory()` time (default `lz4`).
 *
 * Plus the two compression helpers that gate purely on the snapshot's
 * own state (without consulting `m_low_memory_mode_`):
 *
 *   * `compress_if_uncompressed()` — first-time compress + log via
 *                                    a caller-supplied logger.
 *   * `decompress_if_compressed()` — re-hydrate the flat LP buffer.
 *
 * `LinearInterface::enable_compression` /
 * `LinearInterface::disable_compression` retain their public surface
 * but become thin facades that gate on `m_low_memory_mode_` then
 * call into this holder.
 *
 * Invariants (asserted in debug, see `validate_consistency()`):
 *   * **S1.**  Default-constructed `LpSnapshotHolder{}` reports
 *              `has_data() == false`, `is_compressed() == false`,
 *              `codec() == CompressionCodec::lz4`.
 *   * **S2.**  `clear()` resets the snapshot to an empty
 *              `LowMemorySnapshot` and leaves the codec untouched.
 *   * **S3.**  `decompress_if_compressed()` is idempotent — calling
 *              it on a non-compressed snapshot is a no-op.
 *   * **S4.**  `compress_if_uncompressed()` returns ``true`` iff this
 *              call performed the first compression (i.e. the
 *              snapshot transitioned from uncompressed to
 *              compressed); subsequent calls return ``false``.
 *   * **S5.**  `set_codec(c)` only changes the codec used by future
 *              `compress_if_uncompressed()` calls.  An already-
 *              compressed snapshot keeps its original codec.
 */

#pragma once

#include <utility>

#include <gtopt/low_memory_snapshot.hpp>
#include <gtopt/memory_compress.hpp>  // CompressionCodec

namespace gtopt
{

/**
 * @brief Snapshot + codec holder for `LinearInterface` low-memory paths.
 *
 * Pure encapsulation — no reference to `m_low_memory_mode_` or the
 * solver backend.  The `LinearInterface` facade gates on the mode
 * before calling into the holder.
 */
class LpSnapshotHolder
{
public:
  // ── Read accessors ──────────────────────────────────────────────────────

  [[nodiscard]] auto snapshot() const noexcept -> const LowMemorySnapshot&
  {
    return m_snapshot_;
  }
  [[nodiscard]] auto snapshot_mut() noexcept -> LowMemorySnapshot&
  {
    return m_snapshot_;
  }
  [[nodiscard]] auto codec() const noexcept -> CompressionCodec
  {
    return m_codec_;
  }
  [[nodiscard]] auto has_data() const noexcept -> bool
  {
    return m_snapshot_.has_data();
  }
  [[nodiscard]] auto is_compressed() const noexcept -> bool
  {
    return m_snapshot_.is_compressed();
  }

  // ── Write helpers ───────────────────────────────────────────────────────

  void set_codec(CompressionCodec c) noexcept { m_codec_ = c; }

  /// **S2** — drop the snapshot to its default-constructed state.
  /// Leaves the codec untouched (callers that want a full reset
  /// should also `set_codec(CompressionCodec::lz4)`).
  void clear() noexcept { m_snapshot_ = LowMemorySnapshot {}; }

  /// Replace the snapshot's flat LP wholesale.  Used by `load_flat`
  /// and `freeze_for_cuts` paths in `LinearInterface`.
  void set_flat_lp(FlatLinearProblem flat_lp) noexcept
  {
    m_snapshot_.flat_lp = std::move(flat_lp);
  }

  // ── Compression lifecycle ───────────────────────────────────────────────

  /// **S3** — re-hydrate the flat LP from the compressed buffer.
  /// No-op when the snapshot is not compressed.
  void decompress_if_compressed() { m_snapshot_.decompress(); }

  /// **S4** — compress the flat LP buffer using `codec()`.
  /// @returns ``true`` iff this call performed the first compression
  ///          (the snapshot transitioned from uncompressed to
  ///          compressed); ``false`` for repeat calls.  Callers that
  ///          want to log the compression ratio inspect the return
  ///          value to fire only on first-compress.
  [[nodiscard]] auto compress_if_uncompressed() -> bool
  {
    const bool was_first_compress = !m_snapshot_.is_compressed();
    m_snapshot_.compress(m_codec_);
    return was_first_compress && m_snapshot_.is_compressed();
  }

  // ── Diagnostic ──────────────────────────────────────────────────────────

  /// **S1** debug self-check.  Returns true iff the holder is in the
  /// default-constructed state.
  [[nodiscard]] auto is_empty() const noexcept -> bool
  {
    return !m_snapshot_.has_data() && m_codec_ == CompressionCodec::lz4;
  }

private:
  LowMemorySnapshot m_snapshot_ {};
  CompressionCodec m_codec_ {CompressionCodec::lz4};
};

}  // namespace gtopt
