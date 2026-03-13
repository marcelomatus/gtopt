/**
 * @file      check_lp.cpp
 * @brief     Implementation of run_check_lp_diagnostic()
 * @date      2026-03-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>  // O_CLOEXEC for pipe2()

// POSIX process-spawning without a command processor (replaces popen/system).
#include <gtopt/check_lp.hpp>
#include <spawn.h>  // posix_spawn, posix_spawn_file_actions_t
#include <spdlog/spdlog.h>
#include <sys/wait.h>  // waitpid
#include <unistd.h>  // pipe2, read, close, STDOUT_FILENO, STDERR_FILENO, environ

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
[[nodiscard]] std::string find_on_path(std::string_view name)
{
  // NOLINTNEXTLINE(concurrency-mt-unsafe) — getenv is safe at startup
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }

  const std::string_view path_view {path_env};
  std::size_t start = 0;

  while (start < path_view.size()) {
    const auto end = path_view.find(':', start);
    const auto len = (end == std::string_view::npos)
        ? (path_view.size() - start)
        : (end - start);
    const std::filesystem::path candidate =
        std::filesystem::path(path_view.substr(start, len)) / name;
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate.string();
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return {};
}

/**
 * @brief RAII wrapper for @c posix_spawn_file_actions_t.
 *
 * Calls @c posix_spawn_file_actions_init in the constructor and
 * @c posix_spawn_file_actions_destroy in the destructor.
 */
struct SpawnFileActions
{
  posix_spawn_file_actions_t value {};

  SpawnFileActions() noexcept
  {
    static_cast<void>(::posix_spawn_file_actions_init(&value));
  }

  ~SpawnFileActions() noexcept
  {
    static_cast<void>(::posix_spawn_file_actions_destroy(&value));
  }

  SpawnFileActions(const SpawnFileActions&) = delete;
  SpawnFileActions& operator=(const SpawnFileActions&) = delete;
  SpawnFileActions(SpawnFileActions&&) = delete;
  SpawnFileActions& operator=(SpawnFileActions&&) = delete;
};

/**
 * @brief RAII owner of a POSIX file descriptor.
 *
 * Calls @c close(fd) on destruction.  Call @c close() explicitly to
 * release the descriptor early (the destructor then becomes a no-op).
 */
struct UniqueFd
{
  int value {-1};

  UniqueFd() = default;

  explicit UniqueFd(int fd) noexcept
      : value {fd}
  {
  }

  ~UniqueFd() noexcept
  {
    if (value >= 0) {
      ::close(value);
    }
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&&) = delete;
  UniqueFd& operator=(UniqueFd&&) = delete;

  /// Close early; subsequent destructor call is a no-op.
  void close() noexcept
  {
    if (value >= 0) {
      ::close(value);
      value = -1;
    }
  }
};

}  // namespace

std::string run_check_lp_diagnostic(const std::string& lp_file,
                                    int timeout_seconds)
{
  // Ensure the LP file path has an extension, if not, add ".lp" by default.
  const std::string lp_path = lp_file.contains('.') ? lp_file : lp_file + ".lp";

  // Locate the gtopt_check_lp binary on PATH. Only check the binary, the
  // lp_file will be checked out by gtopt_check_lp itself. If not found, return
  // empty string (caller should log and skip diagnostics).
  const std::string bin = find_on_path("gtopt_check_lp");
  if (bin.empty()) {
    SPDLOG_DEBUG("check_lp: gtopt_check_lp not found on PATH");
    return {};
  }

  // Build the argument list.  posix_spawn executes the binary directly —
  // no shell is invoked — which eliminates the command-processor warning
  // and removes any shell-injection surface.
  //
  // Effective command:
  //   timeout <N> gtopt_check_lp --quiet --no-color --timeout <N> <file>
  //
  // --quiet ensures the child never blocks for user input and always exits
  // with code 0 (tries every available solver, falls back to NEOS if an
  // e-mail is configured, warns on failures instead of erroring out).
  //
  // The outer `timeout` (GNU coreutils) kills the child if it exceeds the
  // budget; the inner --timeout is the Python-level budget.
  const std::string timeout_str = std::to_string(timeout_seconds);
  const std::string timeout_bin = find_on_path("timeout");

  // Each std::string must remain alive until posix_spawn() returns.
  std::vector<std::string> arg_strings;
  std::string exec_bin;

  if (!timeout_bin.empty()) {
    exec_bin = timeout_bin;
    arg_strings = {
        timeout_bin,
        timeout_str,
        bin,
        "--quiet",
        "--no-color",
        "--timeout",
        timeout_str,
        lp_path,
    };
  } else {
    exec_bin = bin;
    arg_strings = {
        bin,
        "--quiet",
        "--no-color",
        "--timeout",
        timeout_str,
        lp_path,
    };
  }

  SPDLOG_DEBUG("check_lp: spawning: {}", exec_bin);

  // posix_spawn requires a null-terminated char*const[] argv array.
  // The pointed-to strings are owned by arg_strings above.
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(arg_strings.size() + 1);
  for (auto& s : arg_strings) {
    argv_ptrs.push_back(s.data());
  }
  argv_ptrs.push_back(nullptr);

  // Create a pipe: read_fd (parent reads) ← write_fd (child writes).
  std::array<int, 2> pipefd {};
  if (::pipe2(pipefd.data(), O_CLOEXEC) != 0) {
    SPDLOG_DEBUG("check_lp: pipe2() failed");
    return {};
  }
  const UniqueFd read_fd {pipefd[0]};
  UniqueFd write_fd {pipefd[1]};

  // Wire child stdout + stderr to the write end of the pipe.
  SpawnFileActions fa;
  // In child: close the read end (child does not read from its own output).
  static_cast<void>(::posix_spawn_file_actions_addclose(&fa.value, pipefd[0]));
  // In child: redirect stdout → write end.
  static_cast<void>(
      ::posix_spawn_file_actions_adddup2(&fa.value, pipefd[1], STDOUT_FILENO));
  // In child: redirect stderr → write end.
  static_cast<void>(
      ::posix_spawn_file_actions_adddup2(&fa.value, pipefd[1], STDERR_FILENO));
  // In child: close the original write-end fd (now duplicated via dup2).
  static_cast<void>(::posix_spawn_file_actions_addclose(&fa.value, pipefd[1]));

  // Spawn — exec_bin is a validated PATH result; no shell is involved.
  pid_t pid = 0;
  // NOLINTNEXTLINE(concurrency-mt-unsafe) — environ read is safe at startup
  const int spawn_rc = ::posix_spawn(
      &pid,
      exec_bin.c_str(),
      &fa.value,
      nullptr,
      argv_ptrs.data(),
      environ);  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

  // Parent must close its copy of the write end *before* the read loop so
  // that EOF is delivered when the child exits.
  write_fd.close();

  if (spawn_rc != 0) {
    SPDLOG_DEBUG("check_lp: posix_spawn failed (rc={})", spawn_rc);
    return {};
  }

  // Read child output from the pipe.
  std::string result;
  std::array<char, 512> buffer {};
  constexpr std::size_t max_output = 64UZ * 1024UZ;  // 64 KB cap

  while (true) {
    const auto n = ::read(read_fd.value, buffer.data(), buffer.size());
    if (n <= 0) {
      break;
    }
    result.append(buffer.data(), static_cast<std::size_t>(n));
    if (result.size() > max_output) {
      result += "\n... (diagnostic output truncated) ...\n";
      break;
    }
  }

  // Reap the child to avoid zombies.
  int status = 0;
  ::waitpid(pid, &status, 0);

  return result;
}

}  // namespace gtopt
