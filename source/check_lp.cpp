/**
 * @file      check_lp.cpp
 * @brief     Implementation of run_check_lp_diagnostic()
 * @date      2026-03-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <gtopt/check_lp.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/**
 * @brief Locate @p name in PATH directories, returning the full path or empty.
 *
 * Mimics POSIX @c which: iterates over colon-separated PATH entries and
 * returns the first regular file entry whose name matches @p name.
 */
[[nodiscard]] std::string find_on_path(const std::string& name)
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe) — getenv is safe at startup
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }

  std::string path_str {path_env};
  std::size_t start = 0;

  while (start < path_str.size()) {
    const auto end = path_str.find(':', start);
    const auto len = (end == std::string::npos) ? (path_str.size() - start)
                                                 : (end - start);
    const std::filesystem::path candidate =
        std::filesystem::path(path_str.substr(start, len)) / name;
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate.string();
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return {};
}

}  // namespace

std::string run_check_lp_diagnostic(
    const std::string& lp_file, int timeout_seconds)
{
  // Ensure the LP file path has the .lp extension.
  const std::string lp_path =
      lp_file.ends_with(".lp") ? lp_file : lp_file + ".lp";

  // Bail out silently if the file does not exist.
  std::error_code ec;
  if (!std::filesystem::exists(lp_path, ec)) {
    SPDLOG_DEBUG("check_lp: file not found: {}", lp_path);
    return {};
  }

  // Locate the gtopt-check-lp binary on PATH.
  const std::string bin = find_on_path("gtopt-check-lp");
  if (bin.empty()) {
    SPDLOG_DEBUG("check_lp: gtopt-check-lp not found on PATH");
    return {};
  }

  // Build the command:
  //   timeout <N> gtopt-check-lp --analyze-only --no-color --timeout <N> <file>
  // The outer `timeout` ensures the process is killed if it exceeds the
  // budget; the inner --timeout is the Python-level budget used when the
  // analyse-only flag alone is insufficient.
  const std::string cmd = std::format(
      "timeout {} \"{}\" --analyze-only --no-color --timeout {} \"{}\" 2>&1",
      timeout_seconds,
      bin,
      timeout_seconds,
      lp_path);

  SPDLOG_DEBUG("check_lp: running: {}", cmd);

  // NOLINTNEXTLINE(cert-env33-c) — bin was located on PATH; input is trusted
  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    SPDLOG_DEBUG("check_lp: popen failed for: {}", cmd);
    return {};
  }

  std::string result;
  std::array<char, 512> buffer {};
  constexpr std::size_t max_output = 64UZ * 1024UZ;  // 64 KB cap

  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)
         != nullptr)
  {
    result += buffer.data();
    if (result.size() > max_output) {
      result += "\n... (diagnostic output truncated) ...\n";
      break;
    }
  }
  pclose(pipe);  // NOLINT(cert-env33-c)

  return result;
}

}  // namespace gtopt
