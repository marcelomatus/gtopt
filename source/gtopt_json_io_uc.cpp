/**
 * @file      gtopt_json_io_uc.cpp
 * @brief     User constraint loading (split from gtopt_json_io.cpp)
 * @date      2026-04-09
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Contains load_user_constraints() — uses from_json<vector<UserConstraint>>
 * and PamplParser.  Lighter than the other TUs since it doesn't need
 * from_json<Planning>.
 */

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include <daw/daw_read_file.h>
#include <gtopt/gtopt_json_io.hpp>
#include <gtopt/json/json_user_constraint.hpp>
#include <gtopt/pampl_parser.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

std::expected<void, std::string> load_user_constraints(Planning& planning)
{
  std::vector<std::string> uc_paths;
  if (const auto& uc_file = planning.system.user_constraint_file) {
    uc_paths.push_back(*uc_file);
  }
  for (const auto& f : planning.system.user_constraint_files) {
    uc_paths.push_back(f);
  }

  for (const auto& uc_name : uc_paths) {
    auto filepath = std::filesystem::path {uc_name};
    // Resolve relative paths against input_directory
    if (filepath.is_relative() && planning.options.input_directory) {
      auto alt =
          std::filesystem::path {*planning.options.input_directory} / filepath;
      if (std::filesystem::exists(alt)) {
        filepath = std::move(alt);
      }
    }
    const auto ext = filepath.extension().string();

    try {
      // Determine next UID to avoid collisions with inline constraints
      const auto& existing = planning.system.user_constraint_array;
      Uid next_uid = Uid {1};
      for (const auto& uc : existing) {
        if (uc.uid >= next_uid) {
          next_uid = uc.uid + Uid {1};
        }
      }

      if (ext == ".pampl") {
        auto pampl_result =
            PamplParser::parse_file(filepath.string(), next_uid);
        spdlog::info(
            std::format("Loaded {} constraint(s) and {} param(s) from PAMPL"
                        " file '{}'",
                        pampl_result.constraints.size(),
                        pampl_result.params.size(),
                        filepath.string()));

        auto& arr = planning.system.user_constraint_array;
        arr.insert(arr.end(),
                   std::make_move_iterator(pampl_result.constraints.begin()),
                   std::make_move_iterator(pampl_result.constraints.end()));

        auto& parr = planning.system.user_param_array;
        parr.insert(parr.end(),
                    std::make_move_iterator(pampl_result.params.begin()),
                    std::make_move_iterator(pampl_result.params.end()));
      } else {
        // Default: treat as JSON array of UserConstraint
        auto file_content = daw::read_file(filepath.string());
        if (!file_content) {
          return std::unexpected(std::format(
              "Cannot read user_constraint_file '{}'", filepath.string()));
        }
        auto loaded =
            daw::json::from_json<std::vector<UserConstraint>>(*file_content);
        spdlog::info(
            std::format("Loaded {} user constraint(s) from JSON file '{}'",
                        loaded.size(),
                        filepath.string()));

        auto& arr = planning.system.user_constraint_array;
        arr.insert(arr.end(),
                   std::make_move_iterator(loaded.begin()),
                   std::make_move_iterator(loaded.end()));
      }

    } catch (const std::exception& ex) {
      return std::unexpected(
          std::format("Error loading user_constraint_file '{}': {}",
                      filepath.string(),
                      ex.what()));
    }
  }
  return {};
}

}  // namespace gtopt
