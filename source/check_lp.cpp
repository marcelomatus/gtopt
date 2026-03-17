/**
 * @file      check_lp.cpp
 * @brief     Implementation of run_check_lp_diagnostic() and
 * run_check_json_info()
 * @date      2026-03-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Both public functions delegate to the private `spawn_tool()` helper, which
 * provides a single, reusable posix_spawn-based mechanism for launching
 * external gtopt tools safely (no shell, no injection surface, optional
 * timeout wrapper, optional output capture).
 */

#include <array>
#include <filesystem>
#include <format>
#include <ranges>
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

/// Maximum bytes of child output captured before truncation.
constexpr std::size_t kMaxCapturedOutput = 64UZ * 1024UZ;  // 64 KB

/**
 * @brief Result returned by spawn_tool().
 */
struct SpawnResult
{
  /// true iff the child exited normally with status 0.
  bool exited_ok {};
  /// Captured stdout+stderr — populated only when capture_output=true.
  std::string output {};
};

/**
 * @brief Core posix_spawn helper used by all public functions.
 *
 * Locates @p tool_name on PATH, optionally prepends a `timeout <N>` wrapper
 * to prevent indefinite hangs, then spawns the process directly (no shell).
 *
 * When @p capture_output is @c true the child's stdout and stderr are
 * redirected to a pipe and returned in SpawnResult::output.
 * When @p capture_output is @c false the child inherits the parent's stdio
 * (output goes to the terminal or wherever the parent process writes).
 *
 * @param tool_name       Binary name to look up on PATH.
 * @param extra_args      Arguments appended after the binary name.
 * @param capture_output  Capture stdout+stderr instead of inheriting stdio.
 * @param timeout_seconds When >0 and the `timeout` utility is on PATH, wrap
 *                        the child with `timeout <N>` to enforce a deadline.
 * @return SpawnResult with exited_ok set and output populated (if captured).
 */
[[nodiscard]] SpawnResult spawn_tool(std::string_view tool_name,
                                     const std::vector<std::string>& extra_args,
                                     bool capture_output,
                                     int timeout_seconds)
{
  const std::string bin = find_on_path(tool_name);
  if (bin.empty()) {
    SPDLOG_DEBUG("spawn_tool: {} not found on PATH", tool_name);
    return {};
  }

  // Build the argument list — no shell is involved.
  // When a timeout is requested and the `timeout` utility is available,
  // prepend: timeout <N> <bin>
  const std::string timeout_str = std::to_string(timeout_seconds);
  const std::string timeout_bin =
      (timeout_seconds > 0) ? find_on_path("timeout") : std::string {};

  // Each std::string must outlive posix_spawn().
  std::vector<std::string> arg_strings;
  std::string exec_bin;

  if (!timeout_bin.empty()) {
    exec_bin = timeout_bin;
    arg_strings = {timeout_bin, timeout_str, bin};
  } else {
    exec_bin = bin;
    arg_strings = {bin};
  }

  for (const auto& a : extra_args) {
    arg_strings.push_back(a);
  }

  // posix_spawn requires a null-terminated char*const[] argv array.
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(arg_strings.size() + 1);
  for (auto& s : arg_strings) {
    argv_ptrs.push_back(s.data());
  }
  argv_ptrs.push_back(nullptr);

  SPDLOG_DEBUG(
      "spawn_tool: spawning {} (capture={})", exec_bin, capture_output);

  SpawnResult result {};

  if (capture_output) {
    // ── Capture mode: pipe stdout+stderr back to the parent ────────────
    std::array<int, 2> pipefd {};
    if (::pipe2(pipefd.data(), O_CLOEXEC) != 0) {
      SPDLOG_DEBUG("spawn_tool: pipe2() failed");
      return {};
    }
    const UniqueFd read_fd {pipefd[0]};
    UniqueFd write_fd {pipefd[1]};

    // Wire child stdout + stderr to the write end of the pipe.
    SpawnFileActions fa;
    // In child: close the read end.
    static_cast<void>(
        ::posix_spawn_file_actions_addclose(&fa.value, pipefd[0]));
    // In child: redirect stdout → write end.
    static_cast<void>(::posix_spawn_file_actions_adddup2(
        &fa.value, pipefd[1], STDOUT_FILENO));
    // In child: redirect stderr → write end.
    static_cast<void>(::posix_spawn_file_actions_adddup2(
        &fa.value, pipefd[1], STDERR_FILENO));
    // In child: close the original write-end fd (dup2 duplicated it).
    static_cast<void>(
        ::posix_spawn_file_actions_addclose(&fa.value, pipefd[1]));

    pid_t pid = 0;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — environ read is safe at startup
    const int spawn_rc = ::posix_spawn(
        &pid,
        exec_bin.c_str(),
        &fa.value,
        nullptr,
        argv_ptrs.data(),
        environ);  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

    // Parent must close its write-end copy before the read loop so that
    // EOF is delivered when the child exits.
    write_fd.close();

    if (spawn_rc != 0) {
      SPDLOG_DEBUG("spawn_tool: posix_spawn failed (rc={})", spawn_rc);
      return {};
    }

    // Drain the pipe into result.output.
    std::array<char, 512> buffer {};

    while (true) {
      const auto n = ::read(read_fd.value, buffer.data(), buffer.size());
      if (n <= 0) {
        break;
      }
      result.output.append(buffer.data(), static_cast<std::size_t>(n));
      if (result.output.size() > kMaxCapturedOutput) {
        result.output += "\n... (diagnostic output truncated) ...\n";
        break;
      }
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    result.exited_ok = WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0;

  } else {
    // ── Inherit mode: child writes directly to the parent's stdio ──────
    pid_t pid = 0;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — environ read is safe at startup
    const int spawn_rc = ::posix_spawn(
        &pid,
        exec_bin.c_str(),
        nullptr,  // no file-action redirects; inherit parent stdio
        nullptr,
        argv_ptrs.data(),
        environ);  // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

    if (spawn_rc != 0) {
      SPDLOG_DEBUG("spawn_tool: posix_spawn failed (rc={})", spawn_rc);
      return {};
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    result.exited_ok = WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0;
  }

  return result;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

std::string run_check_lp_diagnostic(const std::string& lp_file,
                                    int timeout_seconds,
                                    const SolverOptions& solver_opts)
{
  // Ensure the LP file path has an extension; default to ".lp".
  const std::string lp_path = lp_file.contains('.') ? lp_file : lp_file + ".lp";

  // Effective command:
  //   [timeout <N>] gtopt_check_lp --quiet --no-color --no-ai --timeout <N>
  //                                [--algo <algo>]
  //                                [--optimal-eps <v>] [--feasible-eps <v>]
  //                                [--barrier-eps <v>]
  //                                <file>
  //
  // --quiet ensures the child never blocks for user input and always exits
  // with code 0 (tries every available solver, falls back to NEOS if an
  // e-mail is configured, warns on failures instead of erroring out).
  // --no-ai avoids hanging on network calls to AI providers when the
  // diagnostic is captured programmatically for logging.
  std::vector<std::string> args {
      "--quiet",
      "--no-color",
      "--no-ai",
      "--timeout",
      std::to_string(timeout_seconds),
  };

  // Pass the LP algorithm when the caller specifies one, so gtopt_check_lp
  // uses the same algorithm (barrier, primal, dual) as the gtopt solver.
  const auto algo_name = std::string(lp_algo_name(solver_opts.algorithm));
  if (!algo_name.empty() && algo_name != "unknown") {
    args.emplace_back("--algo");
    args.emplace_back(algo_name);
  }

  // Pass optional tolerance values so gtopt_check_lp uses the same numerical
  // settings as the original gtopt solve.
  if (solver_opts.optimal_eps) {
    args.emplace_back("--optimal-eps");
    args.emplace_back(std::format("{}", *solver_opts.optimal_eps));
  }
  if (solver_opts.feasible_eps) {
    args.emplace_back("--feasible-eps");
    args.emplace_back(std::format("{}", *solver_opts.feasible_eps));
  }
  if (solver_opts.barrier_eps) {
    args.emplace_back("--barrier-eps");
    args.emplace_back(std::format("{}", *solver_opts.barrier_eps));
  }

  args.emplace_back(lp_path);

  const auto result = spawn_tool("gtopt_check_lp",
                                 args,
                                 /*capture_output=*/true,
                                 timeout_seconds);

  return result.output;
}

void log_diagnostic_lines(std::string_view level,
                          std::string_view header,
                          std::string_view diag)
{
  const bool is_error = (level == "error");
  if (is_error) {
    spdlog::error("LP infeasibility diagnostic for {}:", header);
  } else {
    spdlog::info("LP diagnostic for {}:", header);
  }

  // Collect non-empty lines
  std::vector<std::string_view> lines;
  for (const auto line : std::views::split(diag, '\n')) {
    const std::string_view sv {line.begin(), line.end()};
    if (!sv.empty()) {
      lines.push_back(sv);
    }
  }

  const auto total = static_cast<int>(lines.size());
  const bool truncate = (total > kDiagMaxLines);
  const int start = truncate ? (total - kDiagTailLines) : 0;

  if (truncate) {
    if (is_error) {
      spdlog::error(
          "  ... ({} lines total, showing last {}) ...", total, kDiagTailLines);
    } else {
      spdlog::info(
          "  ... ({} lines total, showing last {}) ...", total, kDiagTailLines);
    }
  }

  for (int i = start; i < total; ++i) {
    if (is_error) {
      spdlog::error("  {}", lines[static_cast<std::size_t>(i)]);
    } else {
      spdlog::info("  {}", lines[static_cast<std::size_t>(i)]);
    }
  }
}

std::string run_check_json_info(const std::vector<std::string>& json_files,
                                int timeout_seconds)
{
  if (json_files.empty()) {
    return {};
  }

  // Build extra args: --info --no-color <file1.json> [<file2.json> ...]
  // Normalise each path to carry a .json extension.
  std::vector<std::string> args {"--info", "--no-color"};
  args.reserve(args.size() + json_files.size());
  for (const auto& stem : json_files) {
    std::filesystem::path p {stem};
    if (!p.has_extension()) {
      p.replace_extension(".json");
    }
    args.push_back(p.string());
  }

  // Effective command:
  //   [timeout <N>] gtopt_check_json --info --no-color <file1.json> [...]
  //
  // stdout+stderr are captured and returned so the caller can forward
  // every line through the spdlog INFO stream instead of writing directly
  // to the terminal.
  const auto result = spawn_tool("gtopt_check_json",
                                 args,
                                 /*capture_output=*/true,
                                 timeout_seconds);

  return result.output;
}

}  // namespace gtopt
