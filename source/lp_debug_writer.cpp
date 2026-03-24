/**
 * @file      lp_debug_writer.cpp
 * @brief     Implementation of LpDebugWriter
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtopt/linear_interface.hpp>
#include <gtopt/lp_debug_writer.hpp>
#include <gtopt/work_pool.hpp>
#include <spdlog/spdlog.h>
#include <zlib.h>
#include <zstd.h>

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
    // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c,bugprone-command-processor)
    return std::system(probe.c_str()) == 0;
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
    // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c,bugprone-command-processor)
    return std::system(full.c_str()) == 0;
  } catch (...) {
    return false;
  }
}

/// Codec table — maps codec names to binary commands and output extensions.
struct CodecInfo
{
  const char* name;
  const char* cmd;
  const char* ext;
};
constexpr std::array<CodecInfo, 5> kCodecs {
    {
        {.name = "gzip", .cmd = "gzip -f", .ext = ".gz"},
        {.name = "zstd", .cmd = "zstd --rm -f -q", .ext = ".zst"},
        {.name = "lz4", .cmd = "lz4 -f --rm -q", .ext = ".lz4"},
        {.name = "bzip2", .cmd = "bzip2 -f", .ext = ".bz2"},
        {.name = "xz", .cmd = "xz -f", .ext = ".xz"},
    },
};

/// Try a named codec directly.  Returns the compressed path on success, or
/// an empty string when the binary is unavailable or the command fails.
/// Falls back to inline libzstd for "zstd" and inline zlib for "gzip".
std::string try_codec(const std::string& src_path, const std::string& codec)
{
  const auto* const it = std::ranges::find(kCodecs, codec, &CodecInfo::name);
  if (it == kCodecs.end()) {
    return {};  // codec not in table
  }

  const auto& entry = *it;
  if (command_available(entry.name)) {
    if (run_external(entry.cmd, src_path)) {
      const std::string out = src_path + entry.ext;
      if (std::filesystem::exists(out)) {
        spdlog::debug("LpDebugWriter: compressed {} via {} → {}",
                      src_path,
                      entry.name,
                      out);
        return out;
      }
    }
    spdlog::debug("LpDebugWriter: {} failed for {}", entry.name, src_path);
  } else {
    spdlog::debug("LpDebugWriter: {} binary not found", entry.name);
  }

  // Named binary unavailable or failed — try inline library fallback.
  if (codec == "zstd") {
    const auto result = zstd_lp_file_inline(src_path);
    if (!result.empty()) {
      spdlog::debug(
          "LpDebugWriter: compressed {} via inline libzstd (zstd binary "
          "unavailable)",
          src_path);
      return result;
    }
  }
  if (codec == "gzip") {
    const auto result = gzip_lp_file_inline(src_path);
    if (!result.empty()) {
      spdlog::debug(
          "LpDebugWriter: compressed {} via inline zlib (gzip binary "
          "unavailable)",
          src_path);
      return result;
    }
  }
  return {};  // codec found in table but could not compress
}

/// Try calling gtopt_compress_lp with an optional --codec suggestion.
/// Returns the result path on success, empty string otherwise.
std::string try_gtopt_compress_lp(const std::string& src_path,
                                  const std::string& codec_hint)
{
  if (!command_available("gtopt_compress_lp")) {
    return {};
  }

  // Build command: gtopt_compress_lp --quiet [--codec <hint>] <path>
  std::string cmd = "gtopt_compress_lp --quiet";
  if (!codec_hint.empty()) {
    cmd += " --codec " + codec_hint;
  }
  cmd += " " + src_path + " >/dev/null 2>&1";

  // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c,bugprone-command-processor)
  if (std::system(cmd.c_str()) != 0) {
    spdlog::debug("LpDebugWriter: gtopt_compress_lp returned non-zero for {}",
                  src_path);
    return {};
  }

  // Look for the compressed output (any known extension).
  for (const char* ext : {".gz", ".zst", ".lz4", ".bz2", ".xz"}) {
    const std::string candidate = src_path + ext;
    if (std::filesystem::exists(candidate)) {
      spdlog::debug("LpDebugWriter: compressed {} via gtopt_compress_lp → {}",
                    src_path,
                    candidate);
      return candidate;
    }
  }
  // Compressed in-place with an unknown extension or already removed.
  if (!std::filesystem::exists(src_path)) {
    return src_path;  // original gone → success even without known extension
  }
  return {};
}

}  // namespace

/// Zstd compression level: 3 is the library default, offering a good
/// balance between speed and ratio for LP debug files.
constexpr int kZstdCompressionLevel = 3;

std::string gzip_lp_file_inline(const std::string& src_path)
{
  std::string gz_path = src_path + ".gz";

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

std::string zstd_lp_file_inline(const std::string& src_path)
{
  std::string zst_path = src_path + ".zst";

  std::ifstream src(src_path, std::ios::binary | std::ios::ate);
  if (!src.is_open()) {
    spdlog::warn("LpDebugWriter: cannot open {} for zstd compression",
                 src_path);
    return {};
  }

  const auto file_size = static_cast<std::size_t>(src.tellg());
  src.seekg(0, std::ios::beg);

  std::vector<char> input(file_size);
  if (!src.read(input.data(), static_cast<std::streamsize>(file_size))) {
    spdlog::warn("LpDebugWriter: cannot read {} for zstd compression",
                 src_path);
    return {};
  }
  src.close();

  const auto bound = ZSTD_compressBound(file_size);
  std::vector<char> output(bound);

  const auto compressed_size = ZSTD_compress(
      output.data(), bound, input.data(), file_size, kZstdCompressionLevel);

  if (ZSTD_isError(compressed_size) != 0U) {
    spdlog::warn("LpDebugWriter: zstd compress error for {}: {}",
                 src_path,
                 ZSTD_getErrorName(compressed_size));
    return {};
  }

  std::ofstream out(zst_path, std::ios::binary);
  if (!out.is_open()) {
    spdlog::warn("LpDebugWriter: cannot create zstd file {}", zst_path);
    return {};
  }
  out.write(output.data(), static_cast<std::streamsize>(compressed_size));
  out.close();

  std::error_code ec;
  std::filesystem::remove(src_path, ec);
  if (ec) {
    spdlog::warn(
        "LpDebugWriter: could not remove {}: {}", src_path, ec.message());
  }

  return zst_path;
}

std::string compress_lp_file(const std::string& src_path,
                             const std::string& lp_compression)
{
  // "none" → caller should not have called us, but handle gracefully.
  if (lp_compression == "none") {
    return src_path;
  }

  // ── 1. gtopt_compress_lp (user-configured, any codec) ────────────────────
  {
    const auto result = try_gtopt_compress_lp(src_path, lp_compression);
    if (!result.empty()) {
      return result;
    }
  }

  // ── 2. Named codec — try directly when gtopt_compress_lp not available ───
  if (!lp_compression.empty()) {
    const auto result = try_codec(src_path, lp_compression);
    if (!result.empty()) {
      return result;
    }
    // Codec not recognised or binary missing → warn and continue cascade.
    if (lp_compression != "zstd" && lp_compression != "gzip") {
      spdlog::warn(
          "LpDebugWriter: codec '{}' unavailable for {}; falling back to zstd",
          lp_compression,
          src_path);
    }
  }

  // ── 3. zstd binary ───────────────────────────────────────────────────────
  if (command_available("zstd")) {
    if (run_external("zstd --rm -f -q", src_path)) {
      const std::string zst = src_path + ".zst";
      if (std::filesystem::exists(zst)) {
        spdlog::debug("LpDebugWriter: compressed {} via zstd", src_path);
        return zst;
      }
    }
  }

  // ── 4. Inline libzstd — statically linked, always available ──────────────
  {
    const auto result = zstd_lp_file_inline(src_path);
    if (!result.empty()) {
      spdlog::debug("LpDebugWriter: compressed {} via inline libzstd",
                    src_path);
      return result;
    }
  }

  // ── 5. gzip binary ───────────────────────────────────────────────────────
  if (command_available("gzip")) {
    if (run_external("gzip -f", src_path)) {
      const std::string gz = src_path + ".gz";
      if (std::filesystem::exists(gz)) {
        spdlog::debug("LpDebugWriter: compressed {} via gzip", src_path);
        return gz;
      }
    }
  }

  // ── 6. Inline zlib — secondary fallback ──────────────────────────────────
  {
    const auto result = gzip_lp_file_inline(src_path);
    if (!result.empty()) {
      spdlog::debug("LpDebugWriter: compressed {} via inline zlib", src_path);
      return result;
    }
  }

  // ── 7. Give up — keep the plain .lp file ─────────────────────────────────
  spdlog::warn(
      "LpDebugWriter: no compression available for {}; keeping plain .lp",
      src_path);
  return src_path;
}

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
  auto result = li.write_lp(filepath_stem);
  if (!result) {
    spdlog::warn("{}", result.error().message);
    return;
  }
  const auto lp_path = filepath_stem + ".lp";
  spdlog::debug("LpDebugWriter: saved LP to {}", lp_path);
  compress_async(lp_path);
}

void LpDebugWriter::compress_async(const std::string& lp_path)
{
  // Empty, "none", and "uncompressed" all mean: keep the plain .lp file.
  // A non-empty codec string triggers compression.
  if (m_compression_.empty() || m_compression_ == "none"
      || m_compression_ == "uncompressed")
  {
    return;
  }

  if (m_pool_ != nullptr) {
    // Assign lowest priority to compress tasks (they don't block solving)
    const TaskRequirements kCompressReq {
        .priority = TaskPriority::Low,
        .priority_key = 0,
        .name = {},
    };
    auto fut = m_pool_->submit([lp_path, compr = m_compression_]
                               { return compress_lp_file(lp_path, compr); },
                               kCompressReq);
    if (fut.has_value()) {
      m_compress_futures_.push_back(std::move(*fut));
      return;
    }
  }
  (void)compress_lp_file(lp_path, m_compression_);
}

void LpDebugWriter::drain()
{
  for (auto& f : m_compress_futures_) {
    (void)f.get();
  }
  m_compress_futures_.clear();
}

}  // namespace gtopt
