// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      sddp_infeasible_probe.cpp
 * @brief     Implementation of probe_infeasible_lp — see header.
 *
 * The probe shells out to `cplex` via popen so that the cold solve
 * uses a separate process with its own memory and parameter state;
 * this is the cleanest way to verify whether the LP itself is
 * structurally infeasible (CPLEX agrees) or whether the in-memory
 * verdict is a phantom of gtopt's lifecycle state (CPLEX disagrees).
 *
 * Concurrency: SDDP solves apertures from multiple worker threads, so
 * the probe acquires a global mutex before spawning CPLEX to avoid 16
 * simultaneous cplex processes competing for cores and RAM.
 */

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

#include <gtopt/linear_interface.hpp>
#include <gtopt/sddp_infeasible_probe.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

// The probe is a debug-only diagnostic gated by an env var; the
// global counter + mutex are inherent to its design (rate-limit and
// serialise child processes across all worker threads).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> g_probe_count {0};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex g_probe_mutex;

[[nodiscard]] bool flag_enabled() noexcept
{
  const auto* v = std::getenv("GTOPT_DEBUG_INFEASIBLE_PROBE");
  if (v == nullptr) {
    return false;
  }
  const std::string_view s {v};
  return !s.empty() && s != "0" && s != "false" && s != "off";
}

[[nodiscard]] int max_probes() noexcept
{
  const auto* v = std::getenv("GTOPT_DEBUG_INFEASIBLE_MAX");
  if (v == nullptr) {
    return 20;
  }
  char* end = nullptr;
  const auto n = std::strtol(v, &end, 10);
  return (end != v && n > 0) ? static_cast<int>(n) : 20;
}

[[nodiscard]] std::string dump_dir()
{
  const auto* v = std::getenv("GTOPT_DEBUG_INFEASIBLE_DIR");
  return (v != nullptr && *v != '\0') ? std::string {v} : std::string {"/tmp"};
}

[[nodiscard]] std::string cplex_bin()
{
  const auto* v = std::getenv("CPLEX_BIN");
  return (v != nullptr && *v != '\0') ? std::string {v} : std::string {"cplex"};
}

/// Capture stdout+stderr from a child command.  Returns empty on
/// failure.  Bounded by an internal buffer chunk size.
[[nodiscard]] std::string run_capture(const std::string& cmd)
{
  std::string out;
  // popen is intentional: the whole point of the probe is to verify
  // gtopt's verdict against a fresh out-of-process CPLEX solve.
  // NOLINTNEXTLINE(cert-env33-c, bugprone-command-processor)
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return out;
  }
  std::array<char, 4096> buf {};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
  {
    out.append(buf.data());
  }
  ::pclose(pipe);
  return out;
}

struct FreshVerdict
{
  std::string status {"unknown"};
  std::string objective;
};

[[nodiscard]] FreshVerdict parse_cplex_output(std::string_view out) noexcept
{
  FreshVerdict v;
  if (out.contains("- Optimal")) {
    v.status = "optimal";
    const auto p = out.find("Objective =");
    if (p != std::string_view::npos) {
      const auto rest = out.substr(p + 11);
      const auto eol = rest.find('\n');
      v.objective = std::string {rest.substr(0, eol)};
      // Trim leading whitespace from the captured number.
      while (!v.objective.empty() && std::isspace(v.objective.front()) != 0) {
        v.objective.erase(v.objective.begin());
      }
    }
  } else if (out.contains("- Infeasible") || out.contains("infeasible")
             || out.contains("Infeasible"))
  {
    v.status = "infeasible";
  } else if (out.contains("Unbounded")) {
    v.status = "unbounded";
  }
  return v;
}

}  // namespace

void probe_infeasible_lp(const LinearInterface& li,
                         std::string context,
                         int inmem_status)
{
  if (!flag_enabled()) {
    return;
  }

  // Atomic counter governs total work; bail out cheaply once we have
  // enough samples.
  const auto seq = g_probe_count.fetch_add(1, std::memory_order_relaxed);
  if (seq >= max_probes()) {
    return;
  }

  // Serialise everything else: writing the LP, spawning cplex, and
  // logging.  Multiple worker threads might hit the probe at once;
  // running 16 cplex processes in parallel would be self-defeating
  // and could itself trigger spurious failures.
  const std::scoped_lock lock {g_probe_mutex};

  const auto dir = dump_dir();
  try {
    std::filesystem::create_directories(dir);
  } catch (const std::exception& ex) {
    spdlog::warn("INFEAS-PROBE [{}]: cannot create dump dir '{}': {}",
                 context,
                 dir,
                 ex.what());
    return;
  }

  const auto stem =
      (std::filesystem::path(dir) / std::format("gtopt_infeas_{}", context))
          .string();
  const auto path = stem + ".lp";

  if (auto wr = li.write_lp(stem); !wr) {
    spdlog::warn(
        "INFEAS-PROBE [{}]: write_lp failed: {}", context, wr.error().message);
    return;
  }

  // ``set logfile *`` silences CPLEX's persistent log file (defaults
  // to cplex.log in the cwd, which we never want to inherit
  // between probes).  ``set output clonelog -1`` keeps the noisy
  // clone log out of stdout in barrier mode.
  const auto cmd = std::format(
      "{} -c 'read {}' 'set logfile *' 'set output clonelog -1' 'optimize' "
      "2>&1",
      cplex_bin(),
      path);

  const auto out = run_capture(cmd);
  const auto v = parse_cplex_output(out);

  spdlog::warn(
      "INFEAS-PROBE [{}]: inmem_status={} fresh={} obj={} nrows={} ncols={} "
      "lp={}",
      context,
      inmem_status,
      v.status,
      v.objective.empty() ? std::string {"-"} : v.objective,
      static_cast<int>(li.get_numrows()),
      static_cast<int>(li.get_numcols()),
      path);
}

}  // namespace gtopt
