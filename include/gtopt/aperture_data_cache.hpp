/**
 * @file      aperture_data_cache.hpp
 * @brief     Pre-loaded aperture scenario data for SDDP backward pass
 * @date      Fri Mar 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Loads all parquet files from the aperture_directory at construction
 * time and provides O(1) lookup of flow/profile values by
 * (element_name, scenario_uid, stage, block).  This avoids reopening
 * parquet files during each SDDP backward-pass iteration.
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>

namespace gtopt
{

/**
 * @brief In-memory cache for aperture-specific schedule data.
 *
 * Reads all `*.parquet` files under a given directory tree at
 * construction time.  Each file is expected to have columns:
 *   - `stage` (int32)
 *   - `block` (int32)
 *   - `uid:<N>` (double) — one per scenario UID
 *
 * The file stem (e.g. "RAPEL" from "RAPEL.parquet") is used as
 * the element name key.  The subdirectory name (e.g. "Flow") is
 * used as the class name key.
 */
class ApertureDataCache
{
public:
  ApertureDataCache() = default;

  /// Load all parquet files from the given directory tree.
  /// Ignores non-parquet files and missing directories.
  explicit ApertureDataCache(const std::filesystem::path& aperture_dir);

  /// Look up a value by (class_name, element_name, scenario_uid, stage, block).
  /// Returns std::nullopt if any key is not found.
  [[nodiscard]] auto lookup(std::string_view class_name,
                            std::string_view element_name,
                            Uid scenario_uid,
                            int stage,
                            int block) const -> std::optional<double>;

  /// Check if any data was loaded.
  [[nodiscard]] bool empty() const noexcept { return m_data_.empty(); }

  /// Number of scenario UIDs loaded.
  [[nodiscard]] auto scenario_uids() const -> std::vector<Uid>;

private:
  /// Key: (class_name, element_name, scenario_uid, stage, block)
  struct Key
  {
    Name class_name;
    Name element_name;
    Uid scenario_uid;
    int stage;
    int block;

    auto operator<=>(const Key&) const = default;
  };

  flat_map<Key, double> m_data_;
};

}  // namespace gtopt
