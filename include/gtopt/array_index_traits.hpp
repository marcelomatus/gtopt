/**
 * @file      array_index_traits.hpp
 * @brief     Traits and utilities for reading Arrow-indexed arrays
 * @date      Thu Apr 24 22:05:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides functions to read Arrow tables from CSV and Parquet
 * files, and traits for resolving indexed field schedules into arrays.
 */

#pragma once

#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <gtopt/arrow_index_cache.hpp>
#include <gtopt/arrow_types.hpp>
#include <gtopt/as_label.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/input_traits.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/**
 * @brief Read an Arrow table from a CSV file.
 *
 * Configures the CSV reader to use typed columns for Scenario, Stage and
 * Block UIDs.  Returns an error string on failure.
 */
[[nodiscard]] auto csv_read_table(const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>;

/**
 * @brief Read an Arrow table from a Parquet file.
 *
 * Returns an error string on failure.
 */
[[nodiscard]] auto parquet_read_table(const std::filesystem::path& fpath)
    -> std::expected<ArrowTable, std::string>;

/**
 * @brief True when @p table uses the long layout: it carries both a bare
 *        `uid` column and a `value` column.
 *
 * Mirrors the Python reader sniff in
 * `scripts/gtopt_check_output/_reader.py::dataset_layout`.  The wide layout
 * (one `uid:N` column per element) never has a bare `uid`+`value` pair, so
 * the test is unambiguous.
 */
[[nodiscard]] auto is_long_layout(const ArrowTable& table) -> bool;

/**
 * @brief Pivot a long-layout table `[<index cols…>, uid, value]` into the
 *        wide layout `[<index cols…>, uid:N…]` that the input loaders expect.
 *
 * Index columns are every column other than `uid`/`value` (any subset of
 * `scenario`/`stage`/`block` — broadcast semantics are preserved because an
 * absent index column simply stays absent).  Duplicate `(index…)` keys in
 * the long form collapse to one wide row per distinct key.  Missing
 * `(uid, key)` cells are zero-filled (matching gtopt's sparse long output,
 * which drops exact zeros; plp2gtopt emits dense long so this never fires).
 * The value column type is preserved as `int32` (integer fields such as
 * `Line/active`) or `double` (floating fields); index/uid columns are
 * emitted as `int32`.  This is a pure column-shape transform — it is
 * agnostic to whether `uid` denotes an element (field files) or a scenario
 * (aperture files), so it serves both readers.
 */
[[nodiscard]] auto pivot_long_to_wide(const ArrowTable& table_in) -> ArrowTable;

/**
 * @brief Build the filesystem path for an input table.
 *
 * If @p fname contains '@', the part before '@' is treated as an override
 * class directory; otherwise @p cname is used.
 *
 * @param input_dir Root input directory.
 * @param cname     Class name (used as sub-directory when '@' is absent).
 * @param fname     Field name, possibly containing an '@' separator.
 * @return          Resolved path (without file extension).
 */
[[nodiscard]] std::filesystem::path build_table_path(std::string_view input_dir,
                                                     std::string_view cname,
                                                     std::string_view fname);

/**
 * @brief Try to read a table, preferring @p format and falling back to the
 *        other format on failure.
 *
 * @param fpath   Path without extension.
 * @param format  Preferred format: @c "parquet" or anything else (CSV).
 * @return        Loaded table or an error description.
 */
[[nodiscard]] auto try_read_table(const std::filesystem::path& fpath,
                                  std::string_view format)
    -> std::expected<ArrowTable, std::string>;

/// True when the index builder for these UID axes provides a long-direct
/// (sparse, per-uid) variant.  Only the (Scenario, Stage, Block) reader does
/// today; every other arity falls back to pivoting long input to wide.
template<typename... Uid>
concept HasArrowLongIndex = requires(const ArrowTable& t) {
  UidToArrowIdx<Uid...>::make_arrow_uids_idx_long(t);
};

struct ArrayIndexBase
{
  [[nodiscard]] static auto read_arrow_table(const SystemContext& sc,
                                             std::string_view cname,
                                             std::string_view fname)
      -> ArrowTable;

  template<typename... Uid>
  [[nodiscard]] static constexpr auto get_arrow_index(
      const SystemContext& system_context,
      std::string_view cname,
      std::string_view fname,
      const Id& id,
      auto& array_map,
      auto& table_map)
  {
    using array_map_key = std::decay_t<decltype(array_map)>::key_type;
    using table_map_key = std::decay_t<decltype(table_map)>::key_type;

    const auto& [uid, name] = id;

    // NOTE: cname/fname are already interned into the shared cache's run-lived
    // storage by the make_array_index file_sched branch, so the string_view
    // keys persisted below never dangle (the cache outlives the per-cell
    // build).  Interning is not done here because this function's
    // system_context parameter is the concrete (forward-declared)
    // SystemContext type — a member access would require its full definition.
    const auto array_key = array_map_key {cname, fname, uid};
    SPDLOG_DEBUG("get_arrow_index: key '{} {} {}: {}'",
                 cname,
                 fname,
                 uid,
                 as_string(array_key));

    // Try to find existing array first (cache hit)
    if (auto aiter = array_map.find(array_key); aiter != array_map.end()) {
      SPDLOG_DEBUG("get_arrow_index: found existing array key '{}'",
                   as_string(array_key));
      return aiter->second;
    }
    SPDLOG_DEBUG("get_arrow_index: creating new array key '{}'",
                 as_string(array_key));

    // Cache miss — load the long table once.  Wide-format input is no longer
    // supported: a non-long file raises an explicit "convert to long" error.
    static_assert(HasArrowLongIndex<Uid...>,
                  "input requires a long-direct index builder for this arity");

    auto table = [&]() -> ArrowTable
    {
      const auto table_key = table_map_key {cname, fname};

      if (auto titer = table_map.find(table_key); titer != table_map.end()) {
        return std::get<0>(titer->second);
      }

      auto loaded = read_arrow_table(system_context, cname, fname);

      if (!is_long_layout(loaded)) [[unlikely]] {
        const auto msg = std::format(
            "Input table '{}/{}' is not in long layout (it has no `uid` + "
            "`value` columns).  Wide-format input is no longer supported — "
            "convert the file to long layout (rows of scenario, stage, block, "
            "uid, value) before running, e.g. with the gtopt Python tooling.",
            cname,
            fname);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      // Collapse to a single chunk so the per-uid row indices built below
      // address `value_col->chunk(0)` directly.
      if (auto combined = loaded->CombineChunks(); combined.ok()) {
        loaded = *combined;
      }
      if (!loaded) [[unlikely]] {
        const auto msg =
            std::format("Can't read long table for '{}:{}'", fname, name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }

      auto [res, ok] = table_map.emplace(
          table_key,
          std::tuple {loaded,
                      typename UidToArrowIdx<Uid...>::UidIdx {},
                      ArrowPresentMask {0}});
      if (!ok) [[unlikely]] {
        const auto msg = std::format(
            "Can't insert non-unique table key '{}' '{}'", cname, fname);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }
      return std::get<0>(res->second);
    }();

    // Build the per-uid (key -> row) submaps in ONE scan and pre-fill the
    // array_map for EVERY uid in the file, then return this uid's entry.
    // array = the shared `value` column (single chunk); index = this uid's
    // submap; mask carries kArrowSparseZeroFillBit so absent cells resolve
    // to 0.  No wide table is ever materialised.
    auto value_col = table->GetColumnByName("value");
    if (!value_col) [[unlikely]] {
      const auto msg = std::format(
          "Long input table for '{}/{}' has no 'value' column", cname, fname);
      SPDLOG_CRITICAL(msg);
      throw std::runtime_error(msg);
    }
    auto [groups, long_mask] =
        UidToArrowIdx<Uid...>::make_arrow_uids_idx_long(table);
    for (auto&& [euid, submap] : groups) {
      array_map.emplace(array_map_key {cname, fname, euid},
                        std::tuple {value_col, std::move(submap), long_mask});
    }
    if (auto it = array_map.find(array_key); it != array_map.end()) {
      return it->second;
    }
    // The requested uid has no rows in the long input file.
    const auto msg =
        std::format("Can't find element '{}:{}' in long input table '{}'",
                    name,
                    uid,
                    fname);
    SPDLOG_CRITICAL(msg);
    throw std::runtime_error(msg);
  }
};

template<typename Type, typename Map, typename FSched, typename... Uid>
struct ArrayIndexTraits
    : InputTraits
    , ArrayIndexBase
{
private:
public:
  using uid_tuple = std::tuple<Uid...>;

  using value_type = Type;
  using vector_type = InputTraits::idx_vector_t<value_type, Uid...>;
  using file_sched = FileSched;
  using array_vector_uid_idx_v = InputTraits::array_vector_uid_idx_v<Uid...>;

  static constexpr auto make_array_index(const auto& system_context,
                                         std::string_view class_name,
                                         Map& array_table_map,
                                         const FSched& sched,
                                         const Id& id) -> array_vector_uid_idx_v
  {
    using map_type = array_table_vector_uid_idx_t<Uid...>;

    auto&& [array_map, table_map, vector_idx] =
        std::get<map_type>(array_table_map);

    SPDLOG_DEBUG("make_array_index: class '{}' id '{} {}'",
                 class_name,
                 id.first,
                 id.second);
    auto arrow_index = std::visit(
        Overload {
            [&](const value_type&) -> array_vector_uid_idx_v
            {
              SPDLOG_DEBUG("make_array_index: value class '{}' id '{} {}'",
                           class_name,
                           id.first,
                           id.second);

              return {};
            },
            [&](const vector_type&) -> array_vector_uid_idx_v
            {
              SPDLOG_DEBUG("make_array_index: vector class '{}' id '{} {}'",
                           class_name,
                           id.first,
                           id.second);
              if (!vector_idx) {
                vector_idx = UidToVectorIdx<Uid...>::make_vector_uids_idx(
                    system_context.simulation());
              }
              return {vector_idx};
            },
            [&](const file_sched& fsched) -> array_vector_uid_idx_v
            {
              SPDLOG_DEBUG("make_array_index: fsched {} class '{}' id '{} {}'",
                           fsched,
                           class_name,
                           id.first,
                           id.second);
              // Intern the class/field names into the shared cache's run-lived
              // storage before they become persisted map keys — the incoming
              // views are only valid for this build call, but the cache (and
              // its string_view keys) outlives it.  system_context here is a
              // deduced `const auto&`, so the .simulation() member access is
              // resolved at instantiation (SystemContext is complete there).
              // Runs under array_index_mutex() (held by make_array_index).
              auto& index_cache =
                  system_context.simulation().arrow_index_cache();
              return get_arrow_index<Uid...>(system_context,
                                             index_cache.intern(class_name),
                                             index_cache.intern(fsched),
                                             id,
                                             array_map,
                                             table_map);
            },
        },
        sched);

    return arrow_index;
  }
};

template<typename Type, typename FieldSched, typename... Uid>
constexpr auto make_array_index(const auto& system_context,
                                std::string_view class_name,
                                const FieldSched& sched,
                                const Id& id)
{
  SPDLOG_DEBUG("make_array_index: class '{}' id '{} {}'",
               class_name,
               id.first,
               id.second);

  // The (cname, fname, uid) -> (Stage, Block) arrow index is scene/phase-
  // invariant, so it is cached once on the shared SimulationLP and reused
  // across every per-cell build, rather than rebuilt (and the parquet
  // re-read) per InputContext.  Lock because the per-(scene, phase) SystemLP
  // builds run concurrently; after warm-up each call is a cheap cache hit.
  // The cache access lives here (where `system_context` is a deduced
  // template parameter) so InputContext need not see SimulationLP complete.
  auto& sim = system_context.simulation();
  const std::scoped_lock lock {sim.array_index_mutex()};
  // arrow_index_cache() lazily creates the cache; calling it under the lock
  // serialises that creation too.
  auto& array_table_map = sim.arrow_index_cache().maps;
  using Map = std::remove_reference_t<decltype(array_table_map)>;
  return ArrayIndexTraits<Type, Map, FieldSched, Uid...>::make_array_index(
      system_context, class_name, array_table_map, sched, id);
}

}  // namespace gtopt
