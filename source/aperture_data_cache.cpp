/**
 * @file      aperture_data_cache.cpp
 * @brief     Implementation of ApertureDataCache
 * @date      Fri Mar 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <charconv>
#include <chrono>
#include <set>

#include <arrow/api.h>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/array_index_traits.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

ApertureDataCache::ApertureDataCache(const std::filesystem::path& aperture_dir)
{
  if (!std::filesystem::exists(aperture_dir)) {
    return;
  }

  spdlog::info("ApertureDataCache: scanning '{}'", aperture_dir.string());

  const auto load_start = std::chrono::steady_clock::now();

  // Collect all entries into a vector first, then bulk-insert into
  // the flat_map.  Inserting one-by-one into a flat_map is O(n) per
  // insert (element shifting), making 340K inserts O(n²) — too slow.
  std::vector<std::pair<Key, double>> entries;

  for (const auto& class_entry :
       std::filesystem::directory_iterator(aperture_dir))
  {
    if (!class_entry.is_directory()) {
      continue;
    }
    const auto class_name = class_entry.path().filename().string();

    for (const auto& file_entry :
         std::filesystem::directory_iterator(class_entry.path()))
    {
      if (!file_entry.is_regular_file()
          || file_entry.path().extension() != ".parquet")
      {
        continue;
      }

      const auto element_name = file_entry.path().stem().string();

      // Read parquet file into Arrow table using the project's reader.
      // parquet_read_table expects a stem (no .parquet extension) — it
      // appends .parquet/.parquet.gz/.parquet.zst internally.
      auto stem_path = file_entry.path();
      stem_path.replace_extension();  // strip .parquet
      auto table_result = parquet_read_table(stem_path);
      if (!table_result.has_value()) {
        spdlog::warn("ApertureDataCache: failed to read {}: {}",
                     file_entry.path().string(),
                     table_result.error());
        continue;
      }
      const auto& table = *table_result;

      // Find stage and block columns
      auto stage_col = table->GetColumnByName("stage");
      auto block_col = table->GetColumnByName("block");
      if (!stage_col || !block_col) {
        spdlog::warn("ApertureDataCache: {} missing stage/block columns",
                     file_entry.path().string());
        continue;
      }

      const auto num_rows = table->num_rows();

      // Get stage and block as int32 arrays
      auto stage_arr =
          std::static_pointer_cast<arrow::Int32Array>(stage_col->chunk(0));
      auto block_arr =
          std::static_pointer_cast<arrow::Int32Array>(block_col->chunk(0));

      // Process uid:N columns
      for (int c = 0; c < table->num_columns(); ++c) {
        const auto& col_name = table->schema()->field(c)->name();
        if (!col_name.starts_with("uid:")) {
          continue;
        }

        // Parse scenario UID from column name "uid:123"
        const auto uid_str = col_name.substr(4);
        uid_t raw_uid {0};
        auto [ptr, ec] = std::from_chars(
            uid_str.data(),
            std::next(uid_str.data(),
                      static_cast<std::ptrdiff_t>(uid_str.size())),
            raw_uid);
        if (ec != std::errc {}) {
          continue;
        }
        const ScenarioUid scen_uid {raw_uid};

        auto val_col = table->column(c);
        auto val_arr =
            std::static_pointer_cast<arrow::DoubleArray>(val_col->chunk(0));

        for (int64_t row = 0; row < num_rows; ++row) {
          entries.emplace_back(
              Key {
                  .class_name = Name {class_name},
                  .element_name = Name {element_name},
                  .scenario_uid = scen_uid,
                  .stage_uid = StageUid {stage_arr->Value(row)},
                  .block_uid = BlockUid {block_arr->Value(row)},
              },
              val_arr->Value(row));
        }
      }
    }
  }

  // Sort and bulk-construct the flat_map — O(n log n) vs O(n²)
  std::ranges::sort(
      entries, [](const auto& a, const auto& b) { return a.first < b.first; });
  m_data_.insert(std::sorted_unique, entries.begin(), entries.end());

  const auto load_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - load_start)
                          .count();
  spdlog::info("ApertureDataCache: loaded {} entries from {} ({:.2f}s)",
               m_data_.size(),
               aperture_dir.string(),
               load_s);
}

auto ApertureDataCache::lookup(std::string_view class_name,
                               std::string_view element_name,
                               ScenarioUid scenario_uid,
                               StageUid stage_uid,
                               BlockUid block_uid) const
    -> std::optional<double>
{
  const auto it = m_data_.find(Key {
      .class_name = Name {class_name},
      .element_name = Name {element_name},
      .scenario_uid = scenario_uid,
      .stage_uid = stage_uid,
      .block_uid = block_uid,
  });
  if (it != m_data_.end()) {
    return it->second;
  }
  return std::nullopt;
}

auto ApertureDataCache::scenario_uids() const -> std::vector<ScenarioUid>
{
  std::set<ScenarioUid> uids;
  for (const auto& [key, val] : m_data_) {
    uids.insert(key.scenario_uid);
  }
  return {uids.begin(), uids.end()};
}

}  // namespace gtopt
