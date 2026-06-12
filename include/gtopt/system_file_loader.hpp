/**
 * @file      system_file_loader.hpp
 * @brief     Load a `System` (and optional `model_options`) from an external
 *            Planning JSON file, with cascade-style path resolution.
 * @date      2026-05-27
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Shared by the cascade method (`CascadeLevel::system_file`) and the SDDP
 * aperture-system feature (`*::aperture_system_file`).  A referenced file is a
 * full Planning JSON; only its `.system` (and, for the model-options variant,
 * its `.options.model_options`) is consumed — the rest is intentionally
 * ignored so the parent `simulation` and the caller's option overlays remain
 * authoritative.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <gtopt/basic_types.hpp>
#include <gtopt/model_options.hpp>
#include <gtopt/system.hpp>

namespace gtopt
{

/// Resolve which aperture-system file applies, given the per-phase override
/// and the already-resolved global default (the cascade-level tier is folded
/// into @p global_file upstream, e.g. by `CascadePlanningMethod`).  An empty
/// string is treated as "unset" so a blank JSON value never shadows the
/// global.  Returns an empty `OptName` when no aperture system applies — the
/// backward pass then falls back to the regular forward system.
///
/// Resolution order realised here: `phase_file` (highest) → `global_file`.
[[nodiscard]] constexpr auto resolve_aperture_system_file(
    const OptName& phase_file, const OptName& global_file) noexcept -> OptName
{
  if (phase_file.has_value() && !phase_file->empty()) {
    return phase_file;
  }
  if (global_file.has_value() && !global_file->empty()) {
    return global_file;
  }
  return OptName {};
}

/// Resolve a relative `file` path the way `parse_planning_files` resolves
/// planning JSONs: try the path as given (relative to CWD) first, then fall
/// back to `input_directory / filename`.  Absolute paths are used as-is.
/// Returns the best candidate; the caller surfaces a clear error if it does
/// not exist.
[[nodiscard]] std::filesystem::path resolve_system_file(
    std::string_view file, const std::string& input_directory);

/// Load only the `system` block from an external Planning JSON file.
/// The loaded file's `options` and `simulation` blocks are discarded.
/// Throws `std::runtime_error` if the file cannot be found or read.
[[nodiscard]] System load_system_from_file(std::string_view file,
                                           const std::string& input_directory);

/// A loaded external system together with the `model_options` declared in its
/// Planning JSON (`.options.model_options`).  Used by the SDDP aperture-system
/// path, which honours the reduced model's own `model_options` (e.g.
/// `use_single_bus`, `use_kirchhoff=false`) — unlike the cascade
/// `system_file` path, which keeps the level's own overlay authoritative.
struct LoadedSystem
{
  System system {};
  ModelOptions model_options {};
};

/// Load both the `system` and the `options.model_options` blocks from an
/// external Planning JSON file.  Throws `std::runtime_error` on read failure.
[[nodiscard]] LoadedSystem load_system_with_model_options(
    std::string_view file, const std::string& input_directory);

}  // namespace gtopt
