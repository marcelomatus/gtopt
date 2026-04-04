/**
 * @file      solver_registry.cpp
 * @brief     Dynamic solver plugin registry implementation
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <numeric>
#include <stdexcept>

#include <dlfcn.h>
#include <gtopt/solver_registry.hpp>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gtopt
{

namespace
{

/// Resolve the directory containing the current executable.
std::filesystem::path exe_directory()
{
  // Linux: /proc/self/exe
  std::array<char, 4096> buf {};
  const auto len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (len <= 0) {
    return {};
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
  buf[static_cast<size_t>(len)] = '\0';
  return std::filesystem::path(buf.data()).parent_path();
}

/// Join a vector of strings with a separator.
std::string join_strings(const std::vector<std::string>& strs,
                         std::string_view sep)
{
  if (strs.empty()) {
    return {};
  }
  return std::accumulate(std::next(strs.begin()),
                         strs.end(),
                         strs.front(),
                         [sep](const std::string& a, const std::string& b)
                         { return std::format("{}{}{}", a, sep, b); });
}

}  // namespace

SolverRegistry::SolverRegistry()
{
  discover_default_paths();
  validate_loaded_solvers();
}

// Intentionally do NOT dlclose() plugin handles.
// Plugin code may still be referenced by SolverBackend instances
// whose destructors run during static destruction.  Closing the
// plugin library before those destructors would cause segfaults.
// The OS reclaims all resources on process exit anyway.
SolverRegistry::~SolverRegistry() = default;

SolverRegistry& SolverRegistry::instance()
{
  static SolverRegistry registry;
  return registry;
}

void SolverRegistry::discover_default_paths()
{
  // 1. $GTOPT_PLUGIN_DIR environment variable
  if (const auto* env =
          std::getenv("GTOPT_PLUGIN_DIR");  // NOLINT(concurrency-mt-unsafe)
      env != nullptr && *env != '\0')
  {
    discover_plugins(env);
  }

  // 2. <exe_dir>/../lib/gtopt/plugins/  (installed layout)
  const auto exe_dir = exe_directory();
  if (!exe_dir.empty()) {
    discover_plugins(exe_dir / ".." / "lib" / "gtopt" / "plugins");

    // 3. <exe_dir>/plugins/  (build tree — plugins/ next to standalone binary)
    discover_plugins(exe_dir / "plugins");

    // 4. <exe_dir>/../plugins/  (build tree — plugins/ sibling to standalone/)
    discover_plugins(exe_dir / ".." / "plugins");

    // 5. <exe_dir>/  (build tree — plugins alongside binary)
    discover_plugins(exe_dir);
  }

  // 6. /usr/local/lib/gtopt/plugins/
  discover_plugins("/usr/local/lib/gtopt/plugins");
}

void SolverRegistry::discover_plugins(const std::filesystem::path& dir)
{
  std::error_code ec;
  const auto canonical_dir = std::filesystem::weakly_canonical(dir, ec);
  const auto& search_dir = ec ? dir : canonical_dir;

  m_searched_dirs_.push_back(search_dir.string());

  if (!std::filesystem::is_directory(search_dir, ec)) {
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(search_dir, ec))
  {
    if (!entry.is_regular_file()) {
      continue;
    }

    const auto& path = entry.path();
    const auto filename = path.filename().string();

    // Match libgtopt_solver_*.so
    if (filename.starts_with("libgtopt_solver_") && filename.ends_with(".so")) {
      load_plugin(path);
    }
  }
}

bool SolverRegistry::load_plugin(const std::filesystem::path& path)
{
  // dlopen with RTLD_LOCAL so symbols don't leak between plugins
  auto* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const auto* err = ::dlerror();  // NOLINT(concurrency-mt-unsafe) - called
                                    // single-threaded at init
    const auto msg = std::format("Failed to load plugin {}: {}",
                                 path.string(),
                                 (err != nullptr) ? err : "unknown");
    SPDLOG_WARN("{}", msg);
    m_load_errors_.push_back(msg);
    return false;
  }

  // Resolve required symbols
  auto* name_fn = reinterpret_cast<solver_plugin_name_fn>(  // NOLINT
      ::dlsym(handle, "gtopt_plugin_name"));
  auto* names_fn = reinterpret_cast<solver_plugin_names_fn>(  // NOLINT
      ::dlsym(handle, "gtopt_solver_names"));
  auto* factory_fn = reinterpret_cast<solver_backend_factory_fn>(  // NOLINT
      ::dlsym(handle, "gtopt_create_backend"));

  if (name_fn == nullptr || names_fn == nullptr || factory_fn == nullptr) {
    const auto msg =
        std::format("Plugin {} missing required symbols", path.string());
    SPDLOG_WARN("{}", msg);
    m_load_errors_.push_back(msg);
    ::dlclose(handle);
    return false;
  }

  // Check ABI version compatibility.  Plugins built against a different
  // SolverBackend vtable layout would crash at runtime; reject them
  // early with a clear diagnostic instead.
  auto* abi_fn = reinterpret_cast<solver_plugin_abi_version_fn>(  // NOLINT
      ::dlsym(handle, "gtopt_plugin_abi_version"));
  const int plugin_abi = (abi_fn != nullptr) ? abi_fn() : 0;
  if (plugin_abi != k_solver_abi_version) {
    const auto msg = std::format(
        "Plugin {} ABI version mismatch: plugin={}, expected={}. "
        "Rebuild the plugin against the current gtopt headers.",
        path.string(),
        plugin_abi,
        k_solver_abi_version);
    SPDLOG_ERROR("{}", msg);
    m_load_errors_.push_back(msg);
    ::dlclose(handle);
    return false;
  }

  // Check for duplicate plugin name
  const std::string plugin_name = name_fn();
  for (const auto& existing : m_plugins_) {
    if (existing.plugin_name == plugin_name) {
      SPDLOG_DEBUG("Plugin '{}' already loaded, skipping {}",
                   plugin_name,
                   path.string());
      ::dlclose(handle);
      return false;
    }
  }

  // Collect solver names
  std::vector<std::string> solver_names;
  for (const auto* names = names_fn(); *names != nullptr;
       ++names)  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  {
    solver_names.emplace_back(*names);
  }

  SPDLOG_DEBUG("Loaded solver plugin '{}' from {} (solvers: {})",
               plugin_name,
               path.string(),
               join_strings(solver_names, ", "));

  m_plugins_.push_back(PluginHandle {
      .dl_handle = handle,
      .create_fn = factory_fn,
      .plugin_name = plugin_name,
      .solver_names = std::move(solver_names),
  });

  return true;
}

bool SolverRegistry::validate_solver_subprocess(const PluginHandle& plugin,
                                                const std::string& solver_name)
{
  // Fork a child process to test the solver backend.
  // This isolates the parent from SEGFAULTs caused by ABI mismatches
  // in the underlying solver library (e.g. COIN-OR compiled with GCC
  // but the plugin loaded into a Clang-compiled binary).
  const pid_t pid = ::fork();

  if (pid == -1) {
    // fork() failed — cannot validate, assume solver is usable.
    SPDLOG_WARN("fork() failed while validating solver '{}', assuming usable",
                solver_name);
    return true;
  }

  if (pid == 0) {
    // Child process: create a backend and call minimal virtual methods.
    // Any crash here terminates only the child, not the parent.
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - single-threaded child
    auto* backend = plugin.create_fn(solver_name.c_str());
    if (backend == nullptr) {
      ::_exit(1);  // NOLINT(concurrency-mt-unsafe)
    }
    // Exercise the vtable with low-cost accessors.
    const auto name = backend->solver_name();
    const auto inf = backend->infinity();
    (void)name;
    (void)inf;
    delete backend;  // NOLINT(cppcoreguidelines-owning-memory)
    ::_exit(0);  // NOLINT(concurrency-mt-unsafe)
  }

  // Parent: wait for the child to finish.
  int status = 0;
  ::waitpid(pid, &status, 0);

  // NOLINTNEXTLINE(hicpp-signed-bitwise) - POSIX macros use bitwise ops
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return true;
  }

  // Build a diagnostic message.
  std::string detail;
  // NOLINTNEXTLINE(hicpp-signed-bitwise)
  if (WIFSIGNALED(status)) {
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    detail = std::format("signal {}", WTERMSIG(status));
  } else {
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    detail = std::format("exit code {}", WEXITSTATUS(status));
  }
  const auto msg = std::format(
      "Solver '{}' from plugin '{}' failed validation ({}). "
      "This usually indicates an ABI mismatch between the solver "
      "library and the gtopt binary (e.g. mixed gcc/clang compilers).",
      solver_name,
      plugin.plugin_name,
      detail);
  SPDLOG_WARN("{}", msg);

  return false;
}

void SolverRegistry::validate_loaded_solvers()
{
  for (auto& plugin : m_plugins_) {
    std::erase_if(plugin.solver_names,
                  [this, &plugin](const std::string& name)
                  {
                    if (!validate_solver_subprocess(plugin, name)) {
                      m_load_errors_.push_back(
                          std::format("Solver '{}' removed: failed subprocess "
                                      "validation",
                                      name));
                      return true;
                    }
                    return false;
                  });
  }

  // Remove plugins with no remaining solver names.
  std::erase_if(m_plugins_,
                [](const PluginHandle& p) { return p.solver_names.empty(); });
}

std::unique_ptr<SolverBackend> SolverRegistry::create(
    std::string_view solver_name) const
{
  for (const auto& plugin : m_plugins_) {
    for (const auto& name : plugin.solver_names) {
      if (name == solver_name) {
        auto* backend = plugin.create_fn(std::string(solver_name).c_str());
        if (backend == nullptr) {
          throw std::runtime_error(
              std::format("Plugin '{}' failed to create solver '{}'",
                          plugin.plugin_name,
                          solver_name));
        }
        return std::unique_ptr<SolverBackend>(backend);
      }
    }
  }

  // Build helpful error message
  const auto available = available_solvers();
  throw std::runtime_error(
      std::format("Solver '{}' not available. Available solvers: {}",
                  solver_name,
                  available.empty() ? std::string("(none — no plugins loaded)")
                                    : join_strings(available, ", ")));
}

std::vector<std::string> SolverRegistry::available_solvers() const
{
  std::vector<std::string> result;
  for (const auto& plugin : m_plugins_) {
    for (const auto& name : plugin.solver_names) {
      result.push_back(name);
    }
  }
  return result;
}

std::string_view SolverRegistry::default_solver() const
{
  // GTOPT_SOLVER env var overrides the default priority.
  if (const auto* env =
          std::getenv("GTOPT_SOLVER"))  // NOLINT(concurrency-mt-unsafe)
  {
    if (has_solver(env)) {
      return env;
    }
    SPDLOG_WARN("GTOPT_SOLVER='{}' not available, falling back to auto", env);
  }

  // Priority order: highs > cplex > cbc > clp
  static constexpr std::array preferred = {"highs", "cplex", "cbc", "clp"};
  for (const auto* name : preferred) {
    if (has_solver(name)) {
      return name;
    }
  }

  throw std::runtime_error(
      "No solver plugins found.\n"
      "Hints:\n"
      "  - Set GTOPT_PLUGIN_DIR to the directory containing solver plugin "
      "libraries\n"
      "  - Ensure libgtopt_solver_osi.so and/or libgtopt_solver_highs.so are "
      "installed\n"
      "  - Install COIN-OR (coinor-libcbc-dev) for CLP/CBC support\n"
      "  - Install HiGHS for HiGHS support\n"
      "  - Run 'gtopt --lp-solvers' to list available LP solvers");
}

bool SolverRegistry::has_solver(std::string_view name) const
{
  return std::ranges::any_of(m_plugins_,
                             [name](const PluginHandle& plugin)
                             {
                               return std::ranges::any_of(
                                   plugin.solver_names,
                                   [name](const std::string& s)
                                   { return s == name; });
                             });
}

const std::vector<std::string>& SolverRegistry::searched_directories() const
{
  return m_searched_dirs_;
}

const std::vector<std::string>& SolverRegistry::load_errors() const
{
  return m_load_errors_;
}

}  // namespace gtopt
