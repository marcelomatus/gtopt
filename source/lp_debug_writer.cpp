/**
 * @file      lp_debug_writer.cpp
 * @brief     Implementation of LpDebugWriter
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>
#include <zlib.h>

namespace gtopt
{

namespace
{

/// Find a command on PATH by checking the filesystem.
/// Returns true if the command is accessible.
bool command_available(const char* cmd) noexcept
{
  // Use popen to ask the shell (portable and safe for availability checks)
  try {
    // "command -v" is POSIX; "which" is not guaranteed but widely available.
    const std::string probe =
        std::string("command -v ") + cmd + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;  // NOLINT(concurrency-mt-unsafe)
  } catch (...) {
    return false;
  }
}

/// Run an external command with a single file argument.
/// Returns true when the command exits with code 0.
bool run_external(const char* cmd, const std::string& arg) noexcept
{
  try {
    const std::string full = std::string(cmd) + " " + arg + " >/dev/null 2>&1";
    return std::system(full.c_str()) == 0;  // NOLINT(concurrency-mt-unsafe)
  } catch (...) {
    return false;
  }
}

/// Inline gzip compression via zlib — last-resort fallback.
/// Returns the .gz path on success, empty string on failure.
std::string gzip_lp_file_inline(const std::string& src_path)
{
  const std::string gz_path = src_path + ".gz";

  std::ifstream src(src_path, std::ios::binary);
  if (!src.is_open()) {
    spdlog::warn("LpDebugWriter: cannot open {} for gzip compression",
                 src_path);
    return {};
  }

  gzFile gz = gzopen(gz_path.c_str(), "wb");
  if (gz == nullptr) {
    spdlog::warn("LpDebugWriter: cannot create gzip file {}", gz_path);
    return {};
  }

  constexpr std::size_t kBufSize = 65536;
  std::array<char, kBufSize> buf {};
  bool ok = true;
  while (src) {
    src.read(buf.data(), static_cast<std::streamsize>(kBufSize));
    const auto n = static_cast<unsigned int>(src.gcount());
    if (n > 0) {
      if (gzwrite(gz, buf.data(), n) == 0) {
        ok = false;
        break;
      }
    }
  }
  gzclose(gz);

  if (!ok) {
    spdlog::warn("LpDebugWriter: gzip write error for {}", gz_path);
    std::filesystem::remove(gz_path);
    return {};
  }

  src.close();
  std::error_code ec;
  std::filesystem::remove(src_path, ec);
  if (ec) {
    spdlog::warn(
        "LpDebugWriter: could not remove {}: {}", src_path, ec.message());
  }

  return gz_path;
}

/// Compress @p src_path.
///
/// Cascade order:
///   1. gtopt_compress_lp — user-configured script (may use any codec).
///   2. gzip              — universally available fallback.
///   3. inline zlib       — statically linked, always available.
///   4. skip              — no compression; log a warning and continue.
///
/// Never throws, never exits with a non-zero error.  The original file is
/// left unmodified when all compression attempts fail.
std::string compress_lp_file(const std::string& src_path)
{
  // ── 1. gtopt_compress_lp (user-configured, recommended) ─────────────────
  if (command_available("gtopt_compress_lp")) {
    if (run_external("gtopt_compress_lp --quiet", src_path)) {
      // gtopt_compress_lp removes the original and creates the compressed
      // file with the codec-appropriate extension. Look for the result.
      for (const char* ext : {".gz", ".zst", ".lz4", ".bz2", ".xz"}) {
        const std::string candidate = src_path + ext;
        if (std::filesystem::exists(candidate)) {
          spdlog::debug(
              "LpDebugWriter: compressed {} via gtopt_compress_lp → {}",
              src_path,
              candidate);
          return candidate;
        }
      }
      // File may have been compressed in-place with the same name (e.g.
      // brotli), or the original was already removed without a known extension.
      if (!std::filesystem::exists(src_path)) {
        // Assume success even without knowing the exact output name.
        return src_path;
      }
    } else {
      spdlog::debug("LpDebugWriter: gtopt_compress_lp returned non-zero for {}",
                    src_path);
    }
  }

  // ── 2. gzip binary ───────────────────────────────────────────────────────
  if (command_available("gzip")) {
    if (run_external("gzip -f", src_path)) {
      const std::string gz = src_path + ".gz";
      if (std::filesystem::exists(gz)) {
        spdlog::debug("LpDebugWriter: compressed {} via gzip", src_path);
        return gz;
      }
    } else {
      spdlog::debug("LpDebugWriter: gzip returned non-zero for {}", src_path);
    }
  }

  // ── 3. inline zlib ───────────────────────────────────────────────────────
  {
    const auto result = gzip_lp_file_inline(src_path);
    if (!result.empty()) {
      spdlog::debug("LpDebugWriter: compressed {} via inline zlib → {}",
                    src_path,
                    result);
      return result;
    }
  }

  // ── 4. No compression available — keep the original .lp file ─────────────
  spdlog::warn(
      "LpDebugWriter: no compression available for {}; keeping plain .lp",
      src_path);
  return src_path;
}

}  // namespace

LpDebugWriter::LpDebugWriter(std::string directory,
                             std::string compression,
                             AdaptiveWorkPool* pool)
    : m_directory_(std::move(directory))
    , m_compression_(std::move(compression))
    , m_pool_(pool)
{
}

bool LpDebugWriter::is_active() const noexcept
{
  return !m_directory_.empty();
}

void LpDebugWriter::ensure_directory()
{
  if (!m_dir_created_) {
    std::filesystem::create_directories(m_directory_);
    m_dir_created_ = true;
  }
}

void LpDebugWriter::write(const LinearInterface& li,
                          const std::string& filepath_stem)
{
  if (!is_active()) {
    return;
  }
  ensure_directory();
  li.write_lp(filepath_stem);
  const auto lp_path = filepath_stem + ".lp";
  spdlog::debug("LpDebugWriter: saved LP to {}", lp_path);
  compress_async(lp_path);
}

void LpDebugWriter::compress_async(const std::string& lp_path)
{
  const bool do_compress =
      !m_compression_.empty() && m_compression_ != "uncompressed";
  if (!do_compress) {
    return;
  }

  if (m_pool_ != nullptr) {
    // Assign lowest priority to compress tasks (they don't block solving)
    const TaskRequirements kCompressReq {
        .priority = TaskPriority::Low,
        .priority_key = 0,
        .name = {},
    };
    auto fut = m_pool_->submit([lp_path] { return compress_lp_file(lp_path); },
                               kCompressReq);
    if (fut.has_value()) {
      m_compress_futures_.push_back(std::move(*fut));
      return;
    }
  }
  (void)compress_lp_file(lp_path);
}

void LpDebugWriter::drain()
{
  for (auto& f : m_compress_futures_) {
    (void)f.get();
  }
  m_compress_futures_.clear();
}

}  // namespace gtopt
