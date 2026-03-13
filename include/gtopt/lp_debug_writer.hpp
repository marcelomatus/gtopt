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

namespace gtopt
{

class AdaptiveWorkPool;
class LinearInterface;

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
