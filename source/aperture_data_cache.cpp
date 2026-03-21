/**
 * @file      aperture_data_cache.cpp
 * @brief     Implementation of ApertureDataCache
 * @date      Fri Mar 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <charconv>
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

  // Walk subdirectories (class_name) and load parquet files (element_name)
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

      // Read parquet file into Arrow table using the project's reader
      auto table_result = parquet_read_table(file_entry.path());
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
        Uid scen_uid {0};
        auto [ptr, ec] = std::from_chars(
            uid_str.data(), uid_str.data() + uid_str.size(), scen_uid);
        if (ec != std::errc {}) {
          continue;
        }

        auto val_col = table->column(c);
        auto val_arr =
            std::static_pointer_cast<arrow::DoubleArray>(val_col->chunk(0));

        for (int64_t row = 0; row < num_rows; ++row) {
          m_data_[Key {
              .class_name = Name {class_name},
              .element_name = Name {element_name},
              .scenario_uid = scen_uid,
              .stage = stage_arr->Value(row),
              .block = block_arr->Value(row),
          }] = val_arr->Value(row);
        }
      }

      SPDLOG_DEBUG("ApertureDataCache: loaded {}/{} ({} rows)",
                   class_name,
                   element_name,
                   num_rows);
    }
  }

  spdlog::info("ApertureDataCache: loaded {} entries from {}",
               m_data_.size(),
               aperture_dir.string());
}

auto ApertureDataCache::lookup(std::string_view class_name,
                               std::string_view element_name,
                               Uid scenario_uid,
                               int stage,
                               int block) const -> std::optional<double>
{
  const auto it = m_data_.find(Key {
      .class_name = Name {class_name},
      .element_name = Name {element_name},
      .scenario_uid = scenario_uid,
      .stage = stage,
      .block = block,
  });
  if (it != m_data_.end()) {
    return it->second;
  }
  return std::nullopt;
}

auto ApertureDataCache::scenario_uids() const -> std::vector<Uid>
{
  std::set<Uid> uids;
  for (const auto& [key, val] : m_data_) {
    uids.insert(key.scenario_uid);
  }
  return {uids.begin(), uids.end()};
}

}  // namespace gtopt
