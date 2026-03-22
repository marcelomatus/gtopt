/**
 * @file      resolve_planning_args.cpp
 * @brief     Resolve planning file arguments and directory context
 * @date      2026-03-21
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <filesystem>
#include <format>

#include <gtopt/resolve_planning_args.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

[[nodiscard]] std::expected<MainOptions, std::string> resolve_planning_args(
    MainOptions opts)
{
  namespace fs = std::filesystem;

  // ── Directory argument → dir/basename.json ──
  // ── Bare stem / missing extension fallback → stem.json ──
  for (auto& pf : opts.planning_files) {
    const auto path = fs::path(pf);

    if (fs::is_directory(path)) {
      auto dir_name = path.filename().string();
      // Handle trailing slash: path("/foo/bar/").filename() == ""
      if (dir_name.empty()) {
        dir_name = path.parent_path().filename().string();
      }
      const auto json_path = path / (dir_name + ".json");
      if (fs::exists(json_path)) {
        spdlog::info("directory argument '{}' -> planning file '{}'",
                     pf,
                     json_path.string());
        pf = json_path.string();

        // Set directories relative to the given directory (unless already
        // set by the user via CLI).  plp2gtopt writes input_directory="."
        // in the JSON (relative to the JSON location), so when running
        // from a parent directory we must point at the actual dir.
        // Similarly, output_directory="results" in the JSON would resolve
        // relative to CWD, so we prefix it with the dir path.
        if (!opts.input_directory) {
          opts.input_directory = path.string();
        }
        if (!opts.output_directory) {
          opts.output_directory = (path / "results").string();
        }
        if (!opts.log_directory) {
          opts.log_directory = (path / "results" / "logs").string();
        }
        if (!opts.cut_directory) {
          opts.cut_directory = (path / "cuts").string();
        }
      } else {
        // Directory exists but dir/basename.json does not — treat as a
        // bare stem (e.g. "bat4b24" is both a data directory and a stem
        // for "bat4b24.json" in CWD).
        auto with_ext = path;
        with_ext.replace_extension(".json");
        if (fs::exists(with_ext)) {
          spdlog::info(
              "'{}' is a directory but '{}.json' found as sibling", pf, pf);
          pf = with_ext.string();
        } else {
          return std::unexpected(std::format(
              "Directory '{}' given but neither '{}' nor '{}' exist",
              pf,
              json_path.string(),
              with_ext.string()));
        }
      }
    } else if (!fs::exists(path)) {
      // Not a directory and doesn't exist — try appending .json
      auto with_ext = path;
      with_ext.replace_extension(".json");
      if (fs::exists(with_ext)) {
        spdlog::info("'{}' not found, using '{}'", pf, with_ext.string());
        pf = with_ext.string();
      }
      // If still not found, parse_planning_files will report the error.
    }
  }

  // ── Auto-detect CWD as input directory ──
  if (opts.planning_files.empty()) {
    const auto cwd = fs::current_path();
    const auto cwd_name = cwd.filename().string();
    const auto candidate = cwd / (cwd_name + ".json");
    if (fs::exists(candidate)) {
      spdlog::info("auto-detected planning file '{}' in current directory",
                   candidate.filename().string());
      opts.planning_files.push_back(candidate.string());

      // Running inside the input directory: all relative paths in the JSON
      // resolve correctly against CWD, so no directory overrides are needed.
    } else {
      return std::unexpected("no planning files given and no auto-detected "
                             + cwd_name
                             + ".json in current directory; use --help");
    }
  }

  return opts;
}

}  // namespace gtopt
