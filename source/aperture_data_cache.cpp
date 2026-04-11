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
#include <thread>

#include <arrow/api.h>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/array_index_traits.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Info needed to load one parquet file.
struct FileInfo
{
  std::filesystem::path path;
  Name class_name;
  Name element_name;
};

/// Result of loading one parquet file: its ElementKey and pre-built map.
struct FileResult
{
  ApertureDataCache::ElementKey key;
  ApertureDataCache::ElementData data;
};

/// Load a single parquet file and return its entries.
auto load_one_file(const FileInfo& info) -> std::optional<FileResult>
{
  // parquet_read_table expects a stem (no .parquet extension)
  auto stem_path = info.path;
  stem_path.replace_extension();
  auto table_result = parquet_read_table(stem_path);
  if (!table_result.has_value()) {
    spdlog::warn("ApertureDataCache: failed to read {}: {}",
                 info.path.string(),
                 table_result.error());
    return std::nullopt;
  }
  const auto& table = *table_result;

  auto stage_col = table->GetColumnByName("stage");
  auto block_col = table->GetColumnByName("block");
  if (!stage_col || !block_col) {
    spdlog::warn("ApertureDataCache: {} missing stage/block columns",
                 info.path.string());
    return std::nullopt;
  }

  const auto num_rows = table->num_rows();
  auto stage_arr =
      std::static_pointer_cast<arrow::Int32Array>(stage_col->chunk(0));
  auto block_arr =
      std::static_pointer_cast<arrow::Int32Array>(block_col->chunk(0));

  // Count uid: columns to reserve capacity
  int uid_col_count = 0;
  for (int c = 0; c < table->num_columns(); ++c) {
    if (table->schema()->field(c)->name().starts_with("uid:")) {
      ++uid_col_count;
    }
  }

  // Build unordered_map directly — no sorting needed, O(1) per insert.
  ApertureDataCache::ElementData data;
  data.reserve(static_cast<size_t>(num_rows)
               * static_cast<size_t>(uid_col_count));

  for (int c = 0; c < table->num_columns(); ++c) {
    const auto& col_name = table->schema()->field(c)->name();
    if (!col_name.starts_with("uid:")) {
      continue;
    }

    const auto uid_str = col_name.substr(4);
    uid_t raw_uid {0};
    auto [ptr, ec] = std::from_chars(
        uid_str.data(),
        std::next(uid_str.data(), static_cast<std::ptrdiff_t>(uid_str.size())),
        raw_uid);
    if (ec != std::errc {}) {
      continue;
    }
    const ScenarioUid scen_uid = make_uid<Scenario>(raw_uid);

    auto val_col = table->column(c);
    auto val_arr =
        std::static_pointer_cast<arrow::DoubleArray>(val_col->chunk(0));

    for (int64_t row = 0; row < num_rows; ++row) {
      data.try_emplace(
          ApertureDataCache::InnerKey {
              .scenario_uid = scen_uid,
              .stage_uid = StageUid {stage_arr->Value(row)},
              .block_uid = make_uid<Block>(block_arr->Value(row)),
          },
          val_arr->Value(row));
    }
  }

  return FileResult {
      .key =
          {
              .class_name = info.class_name,
              .element_name = info.element_name,
          },
      .data = std::move(data),
  };
}

}  // namespace

ApertureDataCache::ApertureDataCache(const std::filesystem::path& aperture_dir)
{
  if (!std::filesystem::exists(aperture_dir)) {
    return;
  }

  spdlog::info("ApertureDataCache: scanning '{}'", aperture_dir.string());

  const auto load_start = std::chrono::steady_clock::now();

  // Phase 1: collect all file paths (fast, single-threaded)
  std::vector<FileInfo> file_list;
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
      file_list.push_back({
          .path = file_entry.path(),
          .class_name = class_name,
          .element_name = file_entry.path().stem().string(),
      });
    }
  }

  // Phase 2: read parquet files in parallel
  std::vector<std::optional<FileResult>> results(file_list.size());
  const auto hw_threads = std::max(1U, std::thread::hardware_concurrency());
  const auto num_threads =
      std::min(static_cast<size_t>(hw_threads), file_list.size());

  if (num_threads <= 1) {
    // Single-threaded fallback
    for (size_t i = 0; i < file_list.size(); ++i) {
      results[i] = load_one_file(file_list[i]);
    }
  } else {
    // Parallel loading with simple work partitioning
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    const auto chunk_size = (file_list.size() + num_threads - 1) / num_threads;

    for (size_t t = 0; t < num_threads; ++t) {
      const auto begin = t * chunk_size;
      const auto end = std::min(begin + chunk_size, file_list.size());
      if (begin >= end) {
        break;
      }
      threads.emplace_back(
          [&, begin, end]()
          {
            for (size_t i = begin; i < end; ++i) {
              results[i] = load_one_file(file_list[i]);
            }
          });
    }
    for (auto& th : threads) {
      th.join();
    }
  }

  // Phase 3: merge pre-built flat_maps into two-level structure
  // Each file's data is already sorted and deduplicated from Phase 2.
  size_t total_entries = 0;
  for (auto& opt_result : results) {
    if (!opt_result) {
      continue;
    }
    auto& [key, data] = *opt_result;
    total_entries += data.size();

    auto [it, inserted] = m_elements_.try_emplace(key, std::move(data));
    if (!inserted) {
      // Multiple files for the same element: merge
      for (const auto& [k, v] : data) {
        it->second.try_emplace(k, v);
      }
    }
  }

  const auto load_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - load_start)
                          .count();
  spdlog::info(
      "ApertureDataCache: loaded {} entries across {} elements from {} "
      "({:.2f}s, {} threads)",
      total_entries,
      m_elements_.size(),
      aperture_dir.string(),
      load_s,
      num_threads);
}

auto ApertureDataCache::lookup(std::string_view class_name,
                               std::string_view element_name,
                               ScenarioUid scenario_uid,
                               StageUid stage_uid,
                               BlockUid block_uid) const
    -> std::optional<double>
{
  // Outer lookup: find the element's data
  const auto elem_it = m_elements_.find(ElementKey {
      .class_name = Name {class_name},
      .element_name = Name {element_name},
  });
  if (elem_it == m_elements_.end()) {
    return std::nullopt;
  }

  // Inner lookup: find (scenario, stage, block) — O(1) hash lookup
  const auto& data = elem_it->second;
  const auto it = data.find(InnerKey {
      .scenario_uid = scenario_uid,
      .stage_uid = stage_uid,
      .block_uid = block_uid,
  });
  if (it != data.end()) {
    return it->second;
  }
  return std::nullopt;
}

auto ApertureDataCache::scenario_uids() const -> std::vector<ScenarioUid>
{
  std::set<ScenarioUid> uids;
  for (const auto& [elem_key, data] : m_elements_) {
    for (const auto& [inner_key, val] : data) {
      uids.insert(inner_key.scenario_uid);
    }
  }
  return {uids.begin(), uids.end()};
}

}  // namespace gtopt
