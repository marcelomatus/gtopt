/**
 * @file      aperture_data_cache.hpp
 * @brief     Pre-loaded aperture scenario data for SDDP backward pass
 * @date      Fri Mar 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Loads all parquet files from the aperture_directory at construction
 * time and provides fast lookup of flow/profile values by
 * (class_name, element_name, scenario_uid, stage, block).
 *
 * Internally uses a two-level structure:
 *   outer: unordered_map<(class, element), inner>
 *   inner: unordered_map<(scenario, stage, block), double>
 *
 * This avoids storing redundant class/element strings per entry
 * (only ~334 unique pairs vs 1.6M entries) and gives O(1) lookup.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage.hpp>

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
  /// Inner key: pure integer triple — O(1) hashed lookup.
  struct InnerKey
  {
    ScenarioUid scenario_uid;
    StageUid stage_uid;
    BlockUid block_uid;

    bool operator==(const InnerKey&) const = default;
  };

  /// Hash for InnerKey combining three integer hashes.
  struct InnerKeyHash
  {
    auto operator()(const InnerKey& k) const noexcept -> std::size_t
    {
      const std::size_t h1 = std::hash<ScenarioUid> {}(k.scenario_uid);
      const std::size_t h2 = std::hash<StageUid> {}(k.stage_uid);
      const std::size_t h3 = std::hash<BlockUid> {}(k.block_uid);
      // NOLINTBEGIN(hicpp-signed-bitwise)
      auto combined = h1
          ^ ((h2 * std::size_t {0x9e3779b97f4a7c15ULL})
             + std::size_t {0x9e3779b9} + (h1 << 6) + (h1 >> 2));
      return combined
          ^ ((h3 * std::size_t {0x9e3779b97f4a7c15ULL})
             + std::size_t {0x9e3779b9} + (combined << 6) + (combined >> 2));
      // NOLINTEND(hicpp-signed-bitwise)
    }
  };

  /// Outer key: (class_name, element_name) — only ~hundreds of unique pairs.
  struct ElementKey
  {
    Name class_name;
    Name element_name;

    bool operator==(const ElementKey&) const = default;
  };

  /// Hash for ElementKey using string hashes combined.
  struct ElementKeyHash
  {
    auto operator()(const ElementKey& k) const noexcept -> std::size_t
    {
      const std::size_t h1 = std::hash<Name> {}(k.class_name);
      const std::size_t h2 = std::hash<Name> {}(k.element_name);
      // NOLINTBEGIN(hicpp-signed-bitwise)
      return h1
          ^ ((h2 * std::size_t {0x9e3779b97f4a7c15ULL})
             + std::size_t {0x9e3779b9} + (h1 << 6) + (h1 >> 2));
      // NOLINTEND(hicpp-signed-bitwise)
    }
  };

  ApertureDataCache() = default;

  /// Load all parquet files from the given directory tree.
  /// Ignores non-parquet files and missing directories.
  explicit ApertureDataCache(const std::filesystem::path& aperture_dir);

  /// Look up a value by (class_name, element_name, scenario_uid, stage, block).
  /// Returns std::nullopt if any key is not found.
  [[nodiscard]] auto lookup(std::string_view class_name,
                            std::string_view element_name,
                            ScenarioUid scenario_uid,
                            StageUid stage_uid,
                            BlockUid block_uid) const -> std::optional<double>;

  /// Check if any data was loaded.
  [[nodiscard]] bool empty() const noexcept { return m_elements_.empty(); }

  /// All scenario UIDs loaded across all elements.
  [[nodiscard]] auto scenario_uids() const -> std::vector<ScenarioUid>;

  /// Per-element data: unordered_map (~4800 entries), O(1) lookup.
  using ElementData = std::unordered_map<InnerKey, double, InnerKeyHash>;

private:
  std::unordered_map<ElementKey, ElementData, ElementKeyHash> m_elements_;
};

}  // namespace gtopt
