/**
 * @file      system_file_loader.cpp
 * @brief     Implementation of the external-Planning-JSON system loader.
 * @date      2026-05-27
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <format>
#include <stdexcept>
#include <utility>

#include <daw/daw_read_file.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/system_file_loader.hpp>

namespace gtopt
{

std::filesystem::path resolve_system_file(std::string_view file,
                                          const std::string& input_directory)
{
  std::filesystem::path p(file);
  if (p.is_absolute() || std::filesystem::exists(p)) {
    return p;
  }
  if (!input_directory.empty()) {
    auto alt = std::filesystem::path(input_directory) / p.filename();
    if (std::filesystem::exists(alt)) {
      return alt;
    }
  }
  return p;  // caller surfaces a clear error if not found
}

LoadedSystem load_system_with_model_options(std::string_view file,
                                            const std::string& input_directory)
{
  const auto path = resolve_system_file(file, input_directory);
  if (!std::filesystem::exists(path)) {
    throw std::runtime_error(std::format(
        "system_file '{}' not found (resolved to '{}')", file, path.string()));
  }
  const auto contents = daw::read_file(path.string());
  if (!contents) {
    throw std::runtime_error(
        std::format("Failed to read system_file '{}'", path.string()));
  }
  auto loaded = parse_planning_json(contents.value());
  return LoadedSystem {
      .system = std::move(loaded.system),
      .model_options = std::move(loaded.options.model_options),
  };
}

System load_system_from_file(std::string_view file,
                             const std::string& input_directory)
{
  return load_system_with_model_options(file, input_directory).system;
}

}  // namespace gtopt
