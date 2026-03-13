/**
 * @file      lp_debug_writer.cpp
 * @brief     Implementation of LpDebugWriter
 * @date      2026-05-01
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <array>
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

/// Compress @p src_path using gzip, writing @p src_path + ".gz", then remove
/// the original.  Returns the path of the created .gz file on success, or an
/// empty string on failure.
std::string gzip_lp_file(const std::string& src_path)
{
  std::string gz_path = src_path + ".gz";

  std::ifstream src(src_path, std::ios::binary);
  if (!src.is_open()) {
    spdlog::warn("LpDebugWriter: cannot open {} for compression", src_path);
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
    auto fut = m_pool_->submit([lp_path] { return gzip_lp_file(lp_path); });
    if (fut.has_value()) {
      m_compress_futures_.push_back(std::move(*fut));
      return;
    }
  }
  (void)gzip_lp_file(lp_path);
}

void LpDebugWriter::drain()
{
  for (auto& f : m_compress_futures_) {
    (void)f.get();
  }
  m_compress_futures_.clear();
}

}  // namespace gtopt
