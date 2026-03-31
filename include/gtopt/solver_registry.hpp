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
 */

#pragma once

#include <filesystem>
#include <memory>
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
 * @code
 *   auto& reg = SolverRegistry::instance();
 *   auto backend = reg.create("highs");  // loads plugin if needed
 * @endcode
 */
class SolverRegistry
{
public:
  /** @brief Get the global singleton registry.
   *
   * On first call, automatically discovers plugins from standard paths.
   */
  static SolverRegistry& instance();

  /** @brief Create a solver backend by name.
   *
   * @param solver_name  Solver identifier: "clp", "cbc", "cplex", "highs"
   * @return Owning pointer to the created backend
   * @throws std::runtime_error if the solver is not available
   */
  [[nodiscard]] std::unique_ptr<SolverBackend> create(
      std::string_view solver_name) const;

  /** @brief Discover and load all plugins from a directory.
   *
   * Scans @p dir for files matching `libgtopt_solver_*.so` and loads each.
   * Already-loaded plugins are skipped.
   */
  void discover_plugins(const std::filesystem::path& dir);

  /** @brief Load a single plugin shared library.
   *
   * @param path  Full path to the .so file
   * @return true if the plugin was loaded successfully
   */
  bool load_plugin(const std::filesystem::path& path);

  /** @brief List all available solver names across loaded plugins. */
  [[nodiscard]] std::vector<std::string> available_solvers() const;

  /** @brief Check whether a solver name is available. */
  [[nodiscard]] bool has_solver(std::string_view name) const;

  /** @brief Return the best available solver name by priority.
   *
   * Priority order: cplex, highs, cbc, clp.
   * @throws std::runtime_error if no solver plugins are loaded.
   */
  [[nodiscard]] std::string_view default_solver() const;

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

  struct PluginHandle
  {
    void* dl_handle {};
    solver_backend_factory_fn create_fn {};
    std::string plugin_name;
    std::vector<std::string> solver_names;
  };

  std::vector<PluginHandle> m_plugins_;
  std::vector<std::string> m_searched_dirs_;
  std::vector<std::string> m_load_errors_;
};

}  // namespace gtopt
