/**
 * @file      uididx_traits.hpp
 * @brief     Header for UID-to-index mapping traits for Arrow tables and
 * vectors
 * @date      Mon Jun  2 22:26:26 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides traits and utilities for building index mappings
 * from UID columns in Arrow tables and simulation vectors.
 */

#pragma once

#include <cstdint>
#include <expected>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <gtopt/arrow_types.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/block.hpp>
#include <gtopt/fmap.hpp>
#include <gtopt/mvector_traits.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/scene.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/utils.hpp>
#include <spdlog/spdlog.h>

namespace gtopt
{

/// Bitmask describing which UID dimensions are present in an input table.
/// Bit `i` corresponds to the `i`-th UID in the parameter pack.  When the
/// bit is 0, the underlying column was absent from the table and the
/// loader has built / will look up the index with a default-constructed
/// UID in that slot — i.e. the value is **broadcast** across that
/// dimension.
using ArrowPresentMask = std::uint32_t;

/// High bit of an ArrowPresentMask flagging a long-direct (sparse) index.
/// When set, a missing `(uid, key)` cell resolves to 0 instead of throwing —
/// matching both the wide pivot's zero-fill and the long format's own
/// zero-drop convention (gtopt long output drops exact zeros).  The low bits
/// (0..2) remain the scenario / stage / block present-axis flags, so
/// `project_key` (which only inspects axis bits) is unaffected.
inline constexpr ArrowPresentMask kArrowSparseZeroFillBit = 1U << 31U;

/// Check whether @p table has a column named @p name.  Returns false on a
/// null table.
[[nodiscard]] inline auto table_has_column(const ArrowTable& table,
                                           std::string_view name) noexcept
    -> bool
{
  if (!table) {
    return false;
  }
  return table->GetColumnByName(std::string {name}) != nullptr;
}
struct UidColumn
{
  [[nodiscard]]
  static auto make_uid_column(const ArrowTable& table, std::string_view name)
      -> std::expected<std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType>,
                       std::string>
  {
    if (!table) {
      auto msg = std::format("Null table, no column for name '{}'", name);
      SPDLOG_ERROR(msg);
      return std::unexpected(std::move(msg));
    }

    const auto column = table->GetColumnByName(std::string {name});
    if (!column) {
      auto msg = std::format("Column '{}' not found in table", name);
      SPDLOG_ERROR(msg);
      return std::unexpected(std::move(msg));
    }

    try {
      if (column->num_chunks() == 0) {
        auto msg = std::format("Column '{}' has no chunks", name);
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }

      // Wide CSVs (e.g. Generator/pmax.csv with >1k UID columns)
      // routinely produce multi-chunk Arrow tables.  The previous
      // implementation returned only `chunk(0)`, and the downstream
      // loop iterated `table->num_rows()` calling `Value(i)` past
      // the chunk's length — reading uninitialized memory and
      // producing spurious zero UIDs that look like duplicate keys.
      // Concatenate all chunks into a single contiguous array so
      // `Value(i)` is well-defined for every `i < num_rows()`.
      std::shared_ptr<arrow::Array> chunk;
      if (column->num_chunks() == 1) {
        chunk = column->chunk(0);
      } else {
        auto concat = arrow::Concatenate(column->chunks());
        if (!concat.ok()) {
          auto msg =
              std::format("Failed to concatenate {} chunks for column '{}': {}",
                          column->num_chunks(),
                          name,
                          concat.status().ToString());
          SPDLOG_ERROR(msg);
          return std::unexpected(std::move(msg));
        }
        chunk = *concat;
      }
      if (!chunk) {
        auto msg = std::format("Null chunk in column '{}'", name);
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }

      if (!is_compatible_int32_type(chunk->type_id())) {
        auto msg =
            std::format("Type mismatch in column '{}': expected {} got {}",
                        name,
                        ArrowTraits<Uid>::Type::type_name(),
                        chunk->type()->ToString());
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }

      auto result = cast_to_int32_array(chunk);
      if (!result) {
        auto msg = std::format("Failed to cast column '{}' to int32", name);
        SPDLOG_ERROR(msg);
        return std::unexpected(std::move(msg));
      }
      return result;
    } catch (const std::exception& e) {
      auto msg = std::format("Column cast failed: {}", e.what());
      SPDLOG_ERROR(msg);
      return std::unexpected(std::move(msg));
    }
  }
};

template<typename Value, typename... Uids>
struct UidMapTraits
{
  using value_type = Value;
  using key_type = std::tuple<Uids...>;
  using uid_map_t = gtopt::flat_map<key_type, value_type>;
  using uid_map_ptr = std::shared_ptr<uid_map_t>;
};

template<typename... Uids>
struct ArrowUidTraits
    : ArrowTraits<Uid>
    , UidMapTraits<ArrowIndex, Uids...>
    , UidColumn
{
  using BaseMapTraits = UidMapTraits<ArrowIndex, Uids...>;

  using typename BaseMapTraits::key_type;
  using uid_arrow_idx_map_t = BaseMapTraits::uid_map_t;
  using uid_arrow_idx_map_ptr = BaseMapTraits::uid_map_ptr;
};

template<typename... Uids>
struct UidToArrowIdx : ArrowUidTraits<Uids...>
{
  using BaseUidTraits = ArrowUidTraits<Uids...>;
  using typename BaseUidTraits::uid_idx_t;
  using typename BaseUidTraits::UidIdx;
};

template<>
struct UidToArrowIdx<ScenarioUid, StageUid, BlockUid>
    : ArrowUidTraits<ScenarioUid, StageUid, BlockUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  /// Build the (scenario, stage, block) → row-index map, broadcasting over
  /// any of the three UID axes whose column is absent from @p table.  Bit
  /// `i` of the returned mask is set when the i-th column (scenario / stage
  /// / block) was found on disk.  Absent dimensions are stored with a
  /// default-constructed UID in the key, which is the same sentinel the
  /// runtime lookup substitutes for broadcast slots.
  static auto make_arrow_uids_idx(const ArrowTable& table)
      -> std::pair<std::shared_ptr<uid_arrow_idx_map_t>, ArrowPresentMask>
  {
    constexpr ArrowPresentMask kScenarioBit = 1U << 0U;
    constexpr ArrowPresentMask kStageBit = 1U << 1U;
    constexpr ArrowPresentMask kBlockBit = 1U << 2U;

    ArrowPresentMask mask = 0;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> scenarios_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> stages_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> blocks_arr;

    if (table_has_column(table, Scenario::class_name)) {
      auto col = make_uid_column(table, Scenario::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get scenarios column {}", col.error());
        return {nullptr, 0};
      }
      scenarios_arr = *col;
      mask |= kScenarioBit;
    }
    if (table_has_column(table, Stage::class_name)) {
      auto col = make_uid_column(table, Stage::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get stages column {}", col.error());
        return {nullptr, 0};
      }
      stages_arr = *col;
      mask |= kStageBit;
    }
    if (table_has_column(table, Block::class_name)) {
      auto col = make_uid_column(table, Block::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get blocks column {}", col.error());
        return {nullptr, 0};
      }
      blocks_arr = *col;
      mask |= kBlockBit;
    }

    if (mask == 0) [[unlikely]] {
      SPDLOG_ERROR(
          "Table has none of the expected UID columns "
          "(scenario, stage, block) — cannot build a (Scenario, Stage, "
          "Block) row index");
      return {nullptr, 0};
    }

    uid_arrow_idx_map_t uid_idx;
    map_reserve(uid_idx, static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto sid = (mask & kScenarioBit)
          ? make_uid<Scenario>(scenarios_arr->Value(i))
          : ScenarioUid {};
      const auto tid = (mask & kStageBit)
          ? make_uid<Stage>(stages_arr->Value(i))
          : StageUid {};
      const auto bid = (mask & kBlockBit)
          ? make_uid<Block>(blocks_arr->Value(i))
          : BlockUid {};
      const auto key = key_type {sid, tid, bid};
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_ERROR(
            "duplicate (Scenario, Stage, Block) key {} at row {} — "
            "input table has more than one row per UID tuple, which "
            "indicates either bad input data or a wrong reader axis "
            "for this schedule (e.g. emitting a `scenario` column on "
            "a 2D (Stage, Block) schedule).  Refusing to silently "
            "shadow the earlier row.",
            as_string(key),
            i);
        throw std::runtime_error(
            "duplicate UID key in input table — see preceding [error] "
            "log line for details");
      }
    }

    return {std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx)), mask};
  }

  /// Long-layout variant.  Groups the rows of a `[scenario?, stage?, block?,
  /// uid, value]` table by the element `uid` column into a per-uid
  /// (Scenario, Stage, Block) -> row-index submap, in a SINGLE scan.  The
  /// caller pairs each submap with the shared `value` column so the runtime
  /// lookup `value = value_col->Value(submap->at(key))` matches the wide
  /// path exactly — without ever materialising a wide table.  The returned
  /// mask carries the same present-axis bits as `make_arrow_uids_idx` plus
  /// `kArrowSparseZeroFillBit`, so absent `(uid, key)` cells resolve to 0.
  ///
  /// The table MUST be single-chunk (callers `CombineChunks` long tables)
  /// so the row indices are valid for `value_col->chunk(0)`.
  static auto make_arrow_uids_idx_long(const ArrowTable& table)
      -> std::pair<gtopt::flat_map<Uid, uid_arrow_idx_map_ptr>,
                   ArrowPresentMask>
  {
    constexpr ArrowPresentMask kScenarioBit = 1U << 0U;
    constexpr ArrowPresentMask kStageBit = 1U << 1U;
    constexpr ArrowPresentMask kBlockBit = 1U << 2U;

    ArrowPresentMask mask = 0;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> scenarios_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> stages_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> blocks_arr;

    if (table_has_column(table, Scenario::class_name)) {
      auto col = make_uid_column(table, Scenario::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: scenarios column {}", col.error());
        return {{}, 0};
      }
      scenarios_arr = *col;
      mask |= kScenarioBit;
    }
    if (table_has_column(table, Stage::class_name)) {
      auto col = make_uid_column(table, Stage::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: stages column {}", col.error());
        return {{}, 0};
      }
      stages_arr = *col;
      mask |= kStageBit;
    }
    if (table_has_column(table, Block::class_name)) {
      auto col = make_uid_column(table, Block::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: blocks column {}", col.error());
        return {{}, 0};
      }
      blocks_arr = *col;
      mask |= kBlockBit;
    }

    if (mask == 0) [[unlikely]] {
      SPDLOG_ERROR(
          "long table has none of the expected UID columns "
          "(scenario, stage, block)");
      return {{}, 0};
    }

    auto uid_col = make_uid_column(table, "uid");
    if (!uid_col) [[unlikely]] {
      SPDLOG_ERROR("long table missing integer 'uid' column: {}",
                   uid_col.error());
      return {{}, 0};
    }
    const auto& uids = *uid_col;

    gtopt::flat_map<Uid, uid_arrow_idx_map_ptr> groups;
    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto sid = (mask & kScenarioBit)
          ? make_uid<Scenario>(scenarios_arr->Value(i))
          : ScenarioUid {};
      const auto tid = (mask & kStageBit)
          ? make_uid<Stage>(stages_arr->Value(i))
          : StageUid {};
      const auto bid = (mask & kBlockBit)
          ? make_uid<Block>(blocks_arr->Value(i))
          : BlockUid {};
      const auto key = key_type {sid, tid, bid};

      const Uid euid = uids->Value(i);
      auto& submap = groups[euid];
      if (!submap) {
        submap = std::make_shared<uid_arrow_idx_map_t>();
      }
      const auto res = submap->emplace(key, i);
      if (!res.second) [[unlikely]] {
        SPDLOG_ERROR(
            "duplicate (Scenario, Stage, Block) key {} for uid {} at row "
            "{} in long input table — more than one row per (uid, scenario, "
            "stage, block) tuple.",
            as_string(key),
            euid,
            i);
        throw std::runtime_error(
            "duplicate UID key in long input table — see preceding [error] "
            "log line for details");
      }
    }

    return {std::move(groups), mask | kArrowSparseZeroFillBit};
  }
};

template<>
struct UidToArrowIdx<StageUid, BlockUid> : ArrowUidTraits<StageUid, BlockUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  /// Build the (stage, block) → row-index map.  When the table only has
  /// a `stage` column (no `block`), the loader broadcasts the per-stage
  /// value across every block by emitting keys of the form
  /// `(stage_uid, BlockUid{})`.  The runtime lookup zeroes the block slot
  /// for the same broadcast effect.  Bit 0 = stage column present,
  /// bit 1 = block column present.
  static auto make_arrow_uids_idx(const ArrowTable& table)
      -> std::pair<std::shared_ptr<uid_arrow_idx_map_t>, ArrowPresentMask>
  {
    constexpr ArrowPresentMask kStageBit = 1U << 0U;
    constexpr ArrowPresentMask kBlockBit = 1U << 1U;

    ArrowPresentMask mask = 0;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> stages_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> blocks_arr;

    if (table_has_column(table, Stage::class_name)) {
      auto col = make_uid_column(table, Stage::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get stages column: {}", col.error());
        return {nullptr, 0};
      }
      stages_arr = *col;
      mask |= kStageBit;
    }
    if (table_has_column(table, Block::class_name)) {
      auto col = make_uid_column(table, Block::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get blocks column {}", col.error());
        return {nullptr, 0};
      }
      blocks_arr = *col;
      mask |= kBlockBit;
    }

    if (mask == 0) [[unlikely]] {
      SPDLOG_ERROR(
          "Table has neither `stage` nor `block` columns — cannot build "
          "a (Stage, Block) row index");
      return {nullptr, 0};
    }

    uid_arrow_idx_map_t uid_idx;
    map_reserve(uid_idx, static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto tid = (mask & kStageBit)
          ? make_uid<Stage>(stages_arr->Value(i))
          : StageUid {};
      const auto bid = (mask & kBlockBit)
          ? make_uid<Block>(blocks_arr->Value(i))
          : BlockUid {};
      const auto key = key_type {tid, bid};
      SPDLOG_DEBUG("uididx Processing key: {} and {}", as_string(key), i);
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        std::string columns;
        for (const auto& n : table->ColumnNames()) {
          if (!columns.empty()) {
            columns += ", ";
          }
          columns += n;
        }
        SPDLOG_ERROR(
            "duplicate (Stage, Block) key {} at row {} — the table has "
            "more than one row per (stage, block) tuple.  Most common "
            "cause: a `scenario` column on a schedule that is read as "
            "2D (Stage, Block).  Either drop the redundant `scenario` "
            "rows in the writer or read it through the 3D (Scenario, "
            "Stage, Block) reader.  Table columns: [{}]",
            as_string(key),
            i,
            columns);
        throw std::runtime_error(
            "duplicate UID key in input table — see preceding [error] "
            "log line for details");
      }
    }

    return {std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx)), mask};
  }

  /// Long-layout variant — see the (Scenario, Stage, Block) overload.  Groups
  /// `[stage?, block?, uid, value]` rows by element uid into per-uid
  /// (Stage, Block) -> row submaps in one scan.  Table must be single-chunk.
  static auto make_arrow_uids_idx_long(const ArrowTable& table)
      -> std::pair<gtopt::flat_map<Uid, uid_arrow_idx_map_ptr>,
                   ArrowPresentMask>
  {
    constexpr ArrowPresentMask kStageBit = 1U << 0U;
    constexpr ArrowPresentMask kBlockBit = 1U << 1U;

    ArrowPresentMask mask = 0;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> stages_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> blocks_arr;

    if (table_has_column(table, Stage::class_name)) {
      auto col = make_uid_column(table, Stage::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: stages column {}", col.error());
        return {{}, 0};
      }
      stages_arr = *col;
      mask |= kStageBit;
    }
    if (table_has_column(table, Block::class_name)) {
      auto col = make_uid_column(table, Block::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: blocks column {}", col.error());
        return {{}, 0};
      }
      blocks_arr = *col;
      mask |= kBlockBit;
    }
    if (mask == 0) [[unlikely]] {
      SPDLOG_ERROR("long table has neither `stage` nor `block` columns");
      return {{}, 0};
    }

    auto uid_col = make_uid_column(table, "uid");
    if (!uid_col) [[unlikely]] {
      SPDLOG_ERROR("long table missing integer 'uid' column: {}",
                   uid_col.error());
      return {{}, 0};
    }
    const auto& uids = *uid_col;

    gtopt::flat_map<Uid, uid_arrow_idx_map_ptr> groups;
    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto tid = (mask & kStageBit)
          ? make_uid<Stage>(stages_arr->Value(i))
          : StageUid {};
      const auto bid = (mask & kBlockBit)
          ? make_uid<Block>(blocks_arr->Value(i))
          : BlockUid {};
      const auto key = key_type {tid, bid};
      const Uid euid = uids->Value(i);
      auto& submap = groups[euid];
      if (!submap) {
        submap = std::make_shared<uid_arrow_idx_map_t>();
      }
      if (!submap->emplace(key, i).second) [[unlikely]] {
        SPDLOG_ERROR(
            "duplicate (Stage, Block) key {} for uid {} at row {} in long "
            "input table",
            as_string(key),
            euid,
            i);
        throw std::runtime_error(
            "duplicate UID key in long input table — see preceding [error] "
            "log line for details");
      }
    }
    return {std::move(groups), mask | kArrowSparseZeroFillBit};
  }
};

template<>
struct UidToArrowIdx<ScenarioUid, StageUid>
    : ArrowUidTraits<ScenarioUid, StageUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  /// Build the (scenario, stage) → row-index map.  When either of the two
  /// UID columns is missing the corresponding axis is broadcast (stored
  /// with a default-constructed UID).  Bit 0 = scenario column present,
  /// bit 1 = stage column present.
  static auto make_arrow_uids_idx(const ArrowTable& table)
      -> std::pair<std::shared_ptr<uid_arrow_idx_map_t>, ArrowPresentMask>
  {
    constexpr ArrowPresentMask kScenarioBit = 1U << 0U;
    constexpr ArrowPresentMask kStageBit = 1U << 1U;

    ArrowPresentMask mask = 0;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> scenarios_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> stages_arr;

    if (table_has_column(table, Scenario::class_name)) {
      auto col = make_uid_column(table, Scenario::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get scenarios column {}", col.error());
        return {nullptr, 0};
      }
      scenarios_arr = *col;
      mask |= kScenarioBit;
    }
    if (table_has_column(table, Stage::class_name)) {
      auto col = make_uid_column(table, Stage::class_name);
      if (!col) {
        SPDLOG_ERROR("Failed to get stages column {}", col.error());
        return {nullptr, 0};
      }
      stages_arr = *col;
      mask |= kStageBit;
    }

    if (mask == 0) [[unlikely]] {
      SPDLOG_ERROR(
          "Table has neither `scenario` nor `stage` columns — cannot "
          "build a (Scenario, Stage) row index");
      return {nullptr, 0};
    }

    uid_arrow_idx_map_t uid_idx;
    map_reserve(uid_idx, static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto sid = (mask & kScenarioBit)
          ? make_uid<Scenario>(scenarios_arr->Value(i))
          : ScenarioUid {};
      const auto tid = (mask & kStageBit)
          ? make_uid<Stage>(stages_arr->Value(i))
          : StageUid {};
      const auto key = key_type {sid, tid};
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_ERROR(
            "duplicate (Scenario, Stage) key {} at row {} — input "
            "table has more than one row per (scenario, stage) tuple.",
            as_string(key),
            i);
        throw std::runtime_error(
            "duplicate UID key in input table — see preceding [error] "
            "log line for details");
      }
    }

    return {std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx)), mask};
  }

  /// Long-layout variant — see the (Scenario, Stage, Block) overload.  Groups
  /// `[scenario?, stage?, uid, value]` rows by element uid into per-uid
  /// (Scenario, Stage) -> row submaps in one scan.  Table must be single-chunk.
  static auto make_arrow_uids_idx_long(const ArrowTable& table)
      -> std::pair<gtopt::flat_map<Uid, uid_arrow_idx_map_ptr>,
                   ArrowPresentMask>
  {
    constexpr ArrowPresentMask kScenarioBit = 1U << 0U;
    constexpr ArrowPresentMask kStageBit = 1U << 1U;

    ArrowPresentMask mask = 0;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> scenarios_arr;
    std::shared_ptr<arrow::CTypeTraits<Uid>::ArrayType> stages_arr;

    if (table_has_column(table, Scenario::class_name)) {
      auto col = make_uid_column(table, Scenario::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: scenarios column {}", col.error());
        return {{}, 0};
      }
      scenarios_arr = *col;
      mask |= kScenarioBit;
    }
    if (table_has_column(table, Stage::class_name)) {
      auto col = make_uid_column(table, Stage::class_name);
      if (!col) {
        SPDLOG_ERROR("long index: stages column {}", col.error());
        return {{}, 0};
      }
      stages_arr = *col;
      mask |= kStageBit;
    }
    if (mask == 0) [[unlikely]] {
      SPDLOG_ERROR("long table has neither `scenario` nor `stage` columns");
      return {{}, 0};
    }

    auto uid_col = make_uid_column(table, "uid");
    if (!uid_col) [[unlikely]] {
      SPDLOG_ERROR("long table missing integer 'uid' column: {}",
                   uid_col.error());
      return {{}, 0};
    }
    const auto& uids = *uid_col;

    gtopt::flat_map<Uid, uid_arrow_idx_map_ptr> groups;
    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto sid = (mask & kScenarioBit)
          ? make_uid<Scenario>(scenarios_arr->Value(i))
          : ScenarioUid {};
      const auto tid = (mask & kStageBit)
          ? make_uid<Stage>(stages_arr->Value(i))
          : StageUid {};
      const auto key = key_type {sid, tid};
      const Uid euid = uids->Value(i);
      auto& submap = groups[euid];
      if (!submap) {
        submap = std::make_shared<uid_arrow_idx_map_t>();
      }
      if (!submap->emplace(key, i).second) [[unlikely]] {
        SPDLOG_ERROR(
            "duplicate (Scenario, Stage) key {} for uid {} at row {} in long "
            "input table",
            as_string(key),
            euid,
            i);
        throw std::runtime_error(
            "duplicate UID key in long input table — see preceding [error] "
            "log line for details");
      }
    }
    return {std::move(groups), mask | kArrowSparseZeroFillBit};
  }
};

template<>
struct UidToArrowIdx<StageUid> : ArrowUidTraits<StageUid>
{
  using UidIdx = uid_arrow_idx_map_ptr;

  /// Build the (stage,) → row-index map.  Bit 0 indicates whether the
  /// `stage` column was present; the single-axis variant always requires
  /// it.
  static auto make_arrow_uids_idx(const ArrowTable& table)
      -> std::pair<std::shared_ptr<uid_arrow_idx_map_t>, ArrowPresentMask>
  {
    constexpr ArrowPresentMask kStageBit = 1U << 0U;

    const auto stages = make_uid_column(table, Stage::class_name);
    if (!stages) {
      SPDLOG_ERROR("Failed to get stages column {}", stages.error());
      return {nullptr, 0};
    }

    uid_arrow_idx_map_t uid_idx;
    map_reserve(uid_idx, static_cast<size_t>(table->num_rows()));

    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto key = key_type {make_uid<Stage>((*stages)->Value(i))};
      const auto res = uid_idx.emplace(key, i);
      if (!res.second) {
        SPDLOG_ERROR(
            "duplicate Stage key {} at row {} — table has more than "
            "one row per stage UID.",
            as_string(key),
            i);
        throw std::runtime_error(
            "duplicate UID key in input table — see preceding [error] "
            "log line for details");
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return {std::make_shared<uid_arrow_idx_map_t>(std::move(uid_idx)),
            kStageBit};
  }

  /// Long-layout variant — see the (Scenario, Stage, Block) overload.  Groups
  /// `[stage, uid, value]` rows by element uid into per-uid (Stage) -> row
  /// submaps in one scan.  Table must be single-chunk.
  static auto make_arrow_uids_idx_long(const ArrowTable& table)
      -> std::pair<gtopt::flat_map<Uid, uid_arrow_idx_map_ptr>,
                   ArrowPresentMask>
  {
    constexpr ArrowPresentMask kStageBit = 1U << 0U;

    const auto stages = make_uid_column(table, Stage::class_name);
    if (!stages) {
      SPDLOG_ERROR("long index: stages column {}", stages.error());
      return {{}, 0};
    }
    auto uid_col = make_uid_column(table, "uid");
    if (!uid_col) {
      SPDLOG_ERROR("long table missing integer 'uid' column: {}",
                   uid_col.error());
      return {{}, 0};
    }
    const auto& uids = *uid_col;

    gtopt::flat_map<Uid, uid_arrow_idx_map_ptr> groups;
    for (ArrowIndex i = 0; i < table->num_rows(); ++i) {
      const auto key = key_type {make_uid<Stage>((*stages)->Value(i))};
      const Uid euid = uids->Value(i);
      auto& submap = groups[euid];
      if (!submap) {
        submap = std::make_shared<uid_arrow_idx_map_t>();
      }
      if (!submap->emplace(key, i).second) [[unlikely]] {
        SPDLOG_ERROR(
            "duplicate Stage key {} for uid {} at row {} in long input table",
            as_string(key),
            euid,
            i);
        throw std::runtime_error(
            "duplicate UID key in long input table — see preceding [error] "
            "log line for details");
      }
    }
    return {std::move(groups), kStageBit | kArrowSparseZeroFillBit};
  }
};

template<typename... Uids>
struct UidToVectorIdx
{
};

template<>
struct UidToVectorIdx<ScenarioUid, StageUid, BlockUid>
{
  using IndexKey = std::tuple<Index, Index, Index>;
  using UidKey = std::tuple<ScenarioUid, StageUid, BlockUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  [[nodiscard]] static auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    map_reserve(index_uids, sim.scenarios().size() * sim.blocks().size());

    for (const auto& [si, scenario] : enumerate<Index>(sim.scenarios())) {
      for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
        for (const auto& [bi, block] : enumerate<Index>(stage.blocks())) {
          const auto res = index_uids.emplace(
              UidKey {scenario.uid(), stage.uid(), block.uid()},
              IndexKey {si, ti, bi});
          if (!res.second) {
            SPDLOG_WARN("using duplicate uid values");
          }
        }
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<StageUid, BlockUid>
{
  using IndexKey = std::tuple<Index, Index>;
  using UidKey = std::tuple<StageUid, BlockUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  static auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    map_reserve(index_uids, sim.blocks().size());

    for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
      for (const auto& [bi, block] : enumerate<Index>(stage.blocks())) {
        const auto res = index_uids.emplace(UidKey {stage.uid(), block.uid()},
                                            IndexKey {ti, bi});
        if (!res.second) {
          SPDLOG_WARN("using duplicate uid values");
        }
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<ScenarioUid, StageUid>
{
  using IndexKey = std::tuple<Index, Index>;
  using UidKey = std::tuple<ScenarioUid, StageUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;

  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  static auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    map_reserve(index_uids, sim.scenarios().size() * sim.stages().size());

    for (const auto& [si, scenario] : enumerate<Index>(sim.scenarios())) {
      for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
        const auto res = index_uids.emplace(
            UidKey {scenario.uid(), stage.uid()}, IndexKey {si, ti});
        if (!res.second) {
          SPDLOG_WARN("Duplicate uid values");
        }
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

template<>
struct UidToVectorIdx<StageUid>
{
  using IndexKey = std::tuple<Index>;
  using UidKey = std::tuple<StageUid>;
  using uid_vector_idx_map_t = gtopt::flat_map<UidKey, IndexKey>;
  using uid_vector_idx_map_ptr = std::shared_ptr<uid_vector_idx_map_t>;
  using UidIdx = uid_vector_idx_map_ptr;

  template<typename SystemContextType = class SystemContext>
  static auto make_vector_uids_idx(const SimulationLP& sim)
  {
    uid_vector_idx_map_t index_uids;
    map_reserve(index_uids, sim.stages().size());

    for (const auto& [ti, stage] : enumerate<Index>(sim.stages())) {
      const auto res = index_uids.emplace(UidKey {stage.uid()}, IndexKey {ti});
      if (!res.second) {
        SPDLOG_WARN("Duplicate uid values");
      }
    }

    // Return a shared pointer to the map containing the index to uid mapping
    return std::make_shared<uid_vector_idx_map_t>(std::move(index_uids));
  }
};

}  // namespace gtopt
