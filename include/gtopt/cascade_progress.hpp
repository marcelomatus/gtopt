/**
 * @file      cascade_progress.hpp
 * @brief     Checkpoint file for cascade-method resume across runs.
 * @date      2026-05-20
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tracks which cascade levels have completed so that `--recover` can
 * restart at the first non-done level rather than always at L0.
 *
 * On every level boundary the cascade method writes
 * `<output_dir>/cascade_progress.json` atomically (write tmp + rename).
 * On `--recover`, the cascade reads it back, identifies the first level
 * whose status is not `done`, and skips earlier levels (loading their
 * persisted cuts files instead of re-solving).
 */
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>

namespace gtopt
{

/// Lifecycle of a single cascade level across runs.
enum class CascadeLevelStatus : std::uint8_t
{
  pending = 0,  ///< Not yet entered (initial state).
  in_progress = 1,  ///< Entered but not finished — partial work on disk.
  done = 2,  ///< Solve finished (converged OR budget exhausted).
};

/// One entry of `CascadeProgress::levels`.
struct CascadeProgressLevel
{
  std::size_t index {0};
  std::string name;
  CascadeLevelStatus status {CascadeLevelStatus::pending};
  bool converged {false};
  int iters {0};
  /// Value of `global_iter_index` just after this level finished;
  /// -1 means "this level has not yet advanced the counter".
  Index global_iter_after {-1};
  /// Path to this level's cuts parquet (resolved by the cascade method).
  std::string cuts_file;
};

/// Top-level checkpoint payload.
struct CascadeProgress
{
  int schema_version {1};
  std::string run_id;
  std::vector<CascadeProgressLevel> levels;
};

/// Load `cascade_progress.json` from disk.
///
/// Returns `FileIOError` if the file is missing or malformed.  Callers
/// should treat "missing file" as cold-start (no recovery available),
/// not as a hard error.
[[nodiscard]] auto load_cascade_progress(const std::filesystem::path& path)
    -> std::expected<CascadeProgress, Error>;

/// Persist `cascade_progress.json` atomically: writes `<path>.tmp` then
/// `std::filesystem::rename(tmp, path)`.  POSIX rename is atomic — the
/// file is either the previous good state or the new good state, never a
/// partial mix.
[[nodiscard]] auto save_cascade_progress(const CascadeProgress& progress,
                                         const std::filesystem::path& path)
    -> std::expected<void, Error>;

}  // namespace gtopt
