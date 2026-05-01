/**
 * @file      solver_registry.hpp
 * @brief     Dynamic solver plugin registry with dlopen support
 * @date      Sun Mar 23 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Manages discovery and loading of solver backend plugins at runtime.
 * Plugins are shared libraries (libgtopt_solver_*.so) that export a
 * standard set of C functions for creating SolverBackend instances.
 *
 * By default the registry uses **lazy loading**: plugin files are
 * discovered (filesystem scan) at construction, but dlopen is deferred
 * until a solver is actually requested.  Call load_all_plugins() to
 * force eager loading (e.g. for --solvers listing).
 */

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtopt/solver_backend.hpp>

namespace gtopt
{

/**
 * @brief Singleton registry for dynamically loaded solver plugins.
 *
 * Usage:
 *
 * @code{.cpp}
 *   auto& reg = SolverRegistry::instance();
 *   auto backend = reg.create("highs");  // loads plugin on demand
 * @endcode
 */
class SolverRegistry
{
public:
  /** @brief Get the global singleton registry.
   *
   * On first call, discovers plugin files from standard paths (but does
   * not dlopen them yet — loading is deferred until needed).
   */
  static SolverRegistry& instance();

  /** @brief Create a solver backend by name.
   *
   * Loads the matching plugin if it has not been loaded yet.
   *
   * @param solver_name  Solver identifier: "clp", "cbc", "cplex", "highs"
   * @return Owning pointer to the created backend
   * @throws std::runtime_error if the solver is not available
   */
  [[nodiscard]] std::unique_ptr<SolverBackend> create(
      std::string_view solver_name);

  /** @brief Discover plugin files in a directory (no dlopen).
   *
   * Scans @p dir for files matching `libgtopt_solver_*.so` and records
   * them for later loading.  Already-recorded paths are skipped.
   */
  void discover_plugins(const std::filesystem::path& dir);

  /** @brief Load a single plugin shared library.
   *
   * @param path  Full path to the .so file
   * @return true if the plugin was loaded successfully
   */
  bool load_plugin(const std::filesystem::path& path);

  /** @brief Force all discovered plugins to be loaded.
   *
   * Call this before available_solvers() or has_solver() when you need
   * to enumerate every solver (e.g. for --solvers).
   */
  void load_all_plugins();

  /** @brief List all available solver names across loaded plugins. */
  [[nodiscard]] std::vector<std::string> available_solvers() const;

  /** @brief Check whether a solver name is available.
   *
   * Only checks already-loaded plugins.  If you need an exhaustive
   * check, call load_all_plugins() first.
   */
  [[nodiscard]] bool has_solver(std::string_view name) const;

  /** @brief Check whether the named solver can solve MIP problems.
   *
   * Loads the plugin on demand if necessary, then queries the backend's
   * supports_mip() method.  Returns false if the solver is not available
   * or does not support integer variables (e.g. CLP).
   */
  [[nodiscard]] bool supports_mip(std::string_view name);

  /** @brief True if at least one loaded backend can solve MIP problems.
   *
   * Calls load_all_plugins() to ensure every available solver has been
   * inspected.  Use this to gate test cases that require a MIP-capable
   * solver — when it returns false, the test should skip.
   */
  [[nodiscard]] bool has_mip_solver();

  /** @brief Return the best available solver name by priority.
   *
   * Loads plugins on demand until a suitable solver is found.
   * Priority order: cplex, highs, mindopt, cbc, clp.
   * @throws std::runtime_error if no solver plugins are loaded.
   */
  [[nodiscard]] std::string_view default_solver();

  /** @brief Return the solver's `+infinity` value WITHOUT instantiating a
   *  `SolverBackend`.
   *
   * Loads the plugin if not already loaded, then queries its
   * `gtopt_solver_infinity` entry point (introduced in this revision).
   * Plugins built before that entry existed return `std::nullopt` —
   * the caller must fall back to creating a backend instance and
   * reading `infinity()` directly.  CPLEX returns 1e+20; HiGHS / OSI /
   * MindOpt / Gurobi return 1e+30.
   *
   * Used by `PlanningLP::auto_scale_*` to skip "no bound" sentinel
   * values (e.g. `Reservoir.fmax = 1e30`) without paying the cost of
   * an extra backend allocation per planning construction.
   *
   * @param solver_name Solver to query (e.g. "cplex", "highs"). Empty
   *                    string defaults to `default_solver()`.
   * @return            Solver's `+infinity` value, or `std::nullopt`
   *                    if the plugin doesn't export the entry point
   *                    or no plugin provides this solver.
   */
  [[nodiscard]] std::optional<double> plugin_infinity(
      std::string_view solver_name = {});

  /** @brief Return the directories that were searched for plugins. */
  [[nodiscard]] const std::vector<std::string>& searched_directories() const;

  /** @brief Return diagnostic messages for plugins that failed to load. */
  [[nodiscard]] const std::vector<std::string>& load_errors() const;

  ~SolverRegistry();

  SolverRegistry(const SolverRegistry&) = delete;
  SolverRegistry& operator=(const SolverRegistry&) = delete;
  SolverRegistry(SolverRegistry&&) = delete;
  SolverRegistry& operator=(SolverRegistry&&) = delete;

private:
  SolverRegistry();
  void discover_default_paths();
  void validate_loaded_solvers();

  /// Check solver availability without locking (caller must hold m_mutex_).
  [[nodiscard]] bool has_solver_unlocked(std::string_view name) const;

  /// Try to load the plugin that provides @p solver_name.
  /// Returns true if the solver is available after loading.
  /// When @p filename_only is true, only try the best-match filename
  /// (libgtopt_solver_{name}.so) without exhaustively loading all plugins.
  bool ensure_solver_loaded(std::string_view solver_name,
                            bool filename_only = false);

  struct PluginHandle
  {
    void* dl_handle {};
    solver_backend_factory_fn create_fn {};
    /// Optional plugin entry: returns the solver's infinity constant
    /// without instantiating a `SolverBackend`.  Resolved via dlsym
    /// at plugin load time (`load_plugin`).  Older plugins built
    /// before this entry point existed report `nullptr`; callers
    /// fall back to instance-level query (create-and-drop a backend
    /// just to read `infinity()`).
    solver_plugin_infinity_fn infinity_fn {};
    std::string plugin_name;
    std::vector<std::string> solver_names;
  };

  [[nodiscard]] static bool validate_solver_subprocess(
      const PluginHandle& plugin, const std::string& solver_name);

  std::vector<PluginHandle> m_plugins_;
  std::vector<std::filesystem::path> m_pending_paths_;
  std::vector<std::string> m_searched_dirs_;
  std::vector<std::string> m_load_errors_;
  mutable std::recursive_mutex m_mutex_;
  bool m_all_loaded_ {false};
};

}  // namespace gtopt
