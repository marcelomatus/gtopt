/**
 * @file      aperture_data_cache.cpp
 * @brief     Implementation of ApertureDataCache
 * @date      Fri Mar 21 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <thread>

#include <arrow/api.h>
#include <gtopt/aperture_data_cache.hpp>
#include <gtopt/array_index_traits.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Read this process's current resident-set size in KiB from
/// ``/proc/self/status``.  Returns 0 on platforms or runtimes where
/// the file is unavailable (procfs not mounted, sandboxed Docker,
/// non-Linux).  Used for one-shot diagnostic deltas around large
/// load operations — not a hot path, so the file open/parse cost
/// is irrelevant.
[[nodiscard]] std::int64_t read_process_vmrss_kib() noexcept
{
  try {
    std::ifstream f("/proc/self/status");
    std::string label;
    while (f >> label) {
      if (label == "VmRSS:") {
        std::int64_t kib {};
        f >> kib;
        return kib;
      }
      f.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
  } catch (...) {
    // Best-effort diagnostic — never let a failed read break the load.
  }
  return 0;
}

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
  // Long-layout aperture files (`[stage, block, uid, value]`, the format
  // plp2gtopt now emits — `uid` is the scenario uid here) are consumed
  // DIRECTLY: each row already maps 1:1 to an `InnerKey{scenario, stage,
  // block} -> value`, so we build the map straight from the long rows.  This
  // avoids the earlier long->wide pivot (`pivot_long_to_wide` +
  // `assemble_wide`), which materialised a full wide table only for the
  // scenario-column loop below to iterate it straight back into this same
  // map — pure round-trip work now that the on-disk format is long.  Fall
  // back to the pivot only for the uncommon non-Int32/Double column types.
  if (is_long_layout(*table_result)) {
    ArrowTable lt = *table_result;
    if (auto combined = lt->CombineChunks(); combined.ok()) {
      lt = *combined;  // single contiguous chunk per column → chunk(0) is safe
    }
    auto ls_stage = lt->GetColumnByName("stage");
    auto ls_block = lt->GetColumnByName("block");
    auto ls_uid = lt->GetColumnByName("uid");  // scenario uid
    auto ls_value = lt->GetColumnByName("value");
    const bool direct_ok = ls_stage && ls_block && ls_uid && ls_value
        && ls_stage->type()->id() == arrow::Type::INT32
        && ls_block->type()->id() == arrow::Type::INT32
        && ls_uid->type()->id() == arrow::Type::INT32
        && ls_value->type()->id() == arrow::Type::DOUBLE;
    if (direct_ok) {
      const auto n_rows = lt->num_rows();
      auto stage_a =
          std::static_pointer_cast<arrow::Int32Array>(ls_stage->chunk(0));
      auto block_a =
          std::static_pointer_cast<arrow::Int32Array>(ls_block->chunk(0));
      auto uid_a =
          std::static_pointer_cast<arrow::Int32Array>(ls_uid->chunk(0));
      auto val_a =
          std::static_pointer_cast<arrow::DoubleArray>(ls_value->chunk(0));
      ApertureDataCache::ElementData data;
      data.reserve(static_cast<size_t>(n_rows));
      for (int64_t row = 0; row < n_rows; ++row) {
        data.try_emplace(
            ApertureDataCache::InnerKey {
                .scenario_uid = make_uid<Scenario>(uid_a->Value(row)),
                .stage_uid = make_uid<Stage>(stage_a->Value(row)),
                .block_uid = make_uid<Block>(block_a->Value(row)),
            },
            val_a->Value(row));
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
  }

  // Wide-layout (or non-standard long) files: use as-is, or pivot the rare
  // non-Int32/Double long file to the wide shape the loop below expects.
  const ArrowTable table = is_long_layout(*table_result)
      ? pivot_long_to_wide(*table_result)
      : *table_result;

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
    auto [ptr, ec] =
        std::from_chars(uid_str.data(),
                        std::next(uid_str.data(), std::ssize(uid_str)),
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
              .stage_uid = make_uid<Stage>(stage_arr->Value(row)),
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
  // Capture process RSS before the load so we can report the delta in
  // the completion line — answers "how much in-memory footprint does
  // the cache actually take?" with a single grep on the gtopt log.
  // Returns 0 on platforms without procfs; the delta line is then
  // suppressed.
  const auto rss_before_kib = read_process_vmrss_kib();

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
    for (const auto& [i, file] : enumerate(file_list)) {
      results[i] = load_one_file(file);
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

  // Phase 4: shrink bucket arrays to the actual entry count.  The
  // per-file `data.reserve(num_rows * uid_col_count)` at line ~80
  // over-allocates because `try_emplace` then dedups; the libstdc++
  // unordered_map keeps the over-allocated bucket array.  Calling
  // `rehash(0)` (== rehash to the minimum size that keeps the load
  // factor below `max_load_factor()`) drops the slack.  On the
  // juan/gtopt_iplp cache (170 k entries across 167 elements) this
  // saves ~5–10 MB at zero lookup cost — the rehash itself is one-
  // shot at startup, ~milliseconds, and is read-only thereafter.
  m_elements_.rehash(0);
  for (auto& [key, data] : m_elements_) {
    data.rehash(0);
  }

  const auto load_s = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - load_start)
                          .count();
  const auto rss_after_kib = read_process_vmrss_kib();
  if (rss_before_kib > 0 && rss_after_kib > 0) {
    const auto delta_mib =
        static_cast<double>(rss_after_kib - rss_before_kib) / 1024.0;
    const auto bytes_per_entry = total_entries > 0
        ? static_cast<double>(rss_after_kib - rss_before_kib) * 1024.0
            / static_cast<double>(total_entries)
        : 0.0;
    spdlog::info(
        "ApertureDataCache: loaded {} entries across {} elements from {} "
        "({:.2f}s, {} threads, ΔRSS={:+.1f} MiB ≈ {:.0f} B/entry)",
        total_entries,
        m_elements_.size(),
        aperture_dir.string(),
        load_s,
        num_threads,
        delta_mib,
        bytes_per_entry);
  } else {
    spdlog::info(
        "ApertureDataCache: loaded {} entries across {} elements from {} "
        "({:.2f}s, {} threads)",
        total_entries,
        m_elements_.size(),
        aperture_dir.string(),
        load_s,
        num_threads);
  }
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
