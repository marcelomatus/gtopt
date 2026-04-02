/**
 * @file      lp_debug_writer.hpp
 * @brief     LpDebugWriter – reusable LP debug file writer with async gzip
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#pragma once

#include <future>
#include <string>
#include <vector>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

class AdaptiveWorkPool;
class LinearInterface;

/// Check whether a (scene_uid, phase_uid) pair falls inside the optional
/// debug filter ranges.  Empty optionals mean "no bound on that side".
[[nodiscard]] constexpr bool in_lp_debug_range(int scene_uid,
                                               int phase_uid,
                                               OptInt scene_min,
                                               OptInt scene_max,
                                               OptInt phase_min,
                                               OptInt phase_max) noexcept
{
  return (!scene_min || scene_uid >= *scene_min)
      && (!scene_max || scene_uid <= *scene_max)
      && (!phase_min || phase_uid >= *phase_min)
      && (!phase_max || phase_uid <= *phase_max);
}

/// @name Compression utility functions (detail — exposed for testing)
/// @{

/// Inline gzip compression via zlib.  Returns .gz path or empty on failure.
[[nodiscard]] std::string gzip_lp_file_inline(const std::string& src_path);

/// Inline zstd compression via libzstd.  Returns .zst path or empty on failure.
[[nodiscard]] std::string zstd_lp_file_inline(const std::string& src_path);

/// Compress a file using the named codec or auto-cascade.
/// @param src_path        Path to the source file to compress.
/// @param lp_compression  "none", "", "gzip", "zstd", "lz4", "bzip2", "xz"
[[nodiscard]] std::string compress_lp_file(
    const std::string& src_path, const std::string& lp_compression = {});

/// @}

/// Encapsulates LP debug file writing and optional async gzip compression.
///
/// Construct with a non-empty @p directory to activate.  When @p compression
/// is non-empty and not "uncompressed", each LP file written is gzip-
/// compressed (removing the original `.lp`).  If a @p pool is provided,
/// compression runs as an async task; otherwise it runs synchronously.
class LpDebugWriter
{
public:
  LpDebugWriter() = default;
  explicit LpDebugWriter(std::string directory,
                         std::string compression = {},
                         AdaptiveWorkPool* pool = nullptr);

  /// Returns true when this writer will actually write files.
  [[nodiscard]] bool is_active() const noexcept;

  /// Write LP file synchronously then optionally submit gzip async.
  /// @param li           LP interface whose state to dump.
  /// @param filepath_stem  Full path stem — extension `.lp` is appended.
  void write(const LinearInterface& li, const std::string& filepath_stem);

  /// Submit gzip compression for an already-written LP file.
  /// @param lp_path  Full path including `.lp` extension.
  void compress_async(const std::string& lp_path);

  /// Wait for all pending async compression futures.
  void drain();

private:
  std::string m_directory_ {};
  std::string m_compression_ {};
  AdaptiveWorkPool* m_pool_ {nullptr};
  bool m_dir_created_ {false};
  std::vector<std::future<std::string>> m_compress_futures_ {};

  void ensure_directory();
};

}  // namespace gtopt
