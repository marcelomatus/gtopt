// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      lp_name_spill.cpp
 * @brief     Async, Parquet-backed LP col/row label-metadata store (see
 * header).
 * @copyright BSD-3-Clause
 *
 * Persistence reuses gtopt's existing Apache Arrow / Parquet stack: each cell's
 * label metadata is written as a long-format table
 * (`kind: int8`, `class_ptr: int64`, `class_len: uint8`, `name_ptr: int64`,
 * `name_len: uint8`, `uid: int32`,
 * `context: fixed_size_binary(sizeof(LpContext))`) via
 * `parquet::arrow::WriteTable`, and read back with the canonical
 * `parquet_read_table` helper.  The name string_views are stored as raw
 * `(pointer, length)` because they point at static `constexpr` name constants
 * (see header) and the file is read within the same running process.  No
 * bespoke byte serialisation.
 */
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <gtopt/array_index_traits.hpp>  // parquet_read_table
#include <gtopt/lp_context.hpp>  // LpContext
#include <gtopt/lp_name_spill.hpp>
#include <parquet/arrow/writer.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

namespace
{

/// Width of the opaque `context` blob column: the trivially-copyable
/// `LpContext` variant is memcpy'd in/out wholesale (mirrors the byte codec in
/// `linear_interface_labels_codec.cpp`).
constexpr int kContextWidth = static_cast<int>(sizeof(LpContext));

void check_status(const arrow::Status& status, const char* what)
{
  if (!status.ok()) {
    throw std::runtime_error(std::string(what) + ": " + status.ToString());
  }
}

/// `memcpy` through `void*` params so the (trivially-copyable but
/// non-trivially-assignable) `LpContext` variant does not trip
/// `-Wclass-memaccess` — same technique as `linear_interface_labels_codec.cpp`.
void copy_bytes(void* dst, const void* src, std::size_t n) noexcept
{
  std::memcpy(dst, src, n);
}

/// Encode a (static) name string_view as a raw pointer (`bit_cast`, the
/// sanctioned alternative to `reinterpret_cast`).
[[nodiscard]] std::int64_t ptr_of(std::string_view sv) noexcept
{
  return std::bit_cast<std::int64_t>(sv.data());
}

/// Inverse of `ptr_of`: rebuild the string_view from `(ptr, len)`.  A zero
/// length yields an empty view without dereferencing the (possibly null)
/// pointer.
[[nodiscard]] std::string_view view_of(std::int64_t ptr,
                                       std::size_t len) noexcept
{
  if (len == 0) {
    return {};
  }
  return {std::bit_cast<const char*>(ptr), len};
}

/// Name lengths are stored in a single byte: every class / variable /
/// constraint name is a short static `constexpr` identifier, far under 256
/// bytes.  Guard against the (unexpected) overflow rather than truncate
/// silently — the worker catches the throw and skips that cell's spill.
[[nodiscard]] std::uint8_t encode_len(std::string_view sv)
{
  if (sv.size() > 0xFFU) {
    throw std::runtime_error("lp_name_spill: name exceeds 255 bytes: "
                             + std::string(sv.substr(0, 64)));
  }
  return static_cast<std::uint8_t>(sv.size());
}

/// Build the long-format Arrow table for one cell's label metadata.
[[nodiscard]] std::shared_ptr<arrow::Table> meta_to_table(
    const LpLabelMeta& meta)
{
  arrow::Int8Builder kind_builder;
  arrow::Int64Builder class_ptr_builder;
  arrow::UInt8Builder class_len_builder;
  arrow::Int64Builder name_ptr_builder;
  arrow::UInt8Builder name_len_builder;
  arrow::Int32Builder uid_builder;
  arrow::FixedSizeBinaryBuilder ctx_builder(
      arrow::fixed_size_binary(kContextWidth));

  const auto append = [&](std::int8_t kind,
                          std::string_view class_name,
                          std::string_view name,
                          std::int32_t uid,
                          const LpContext& context)
  {
    check_status(kind_builder.Append(kind), "kind append");
    check_status(class_ptr_builder.Append(ptr_of(class_name)), "class_ptr");
    check_status(class_len_builder.Append(encode_len(class_name)), "class_len");
    check_status(name_ptr_builder.Append(ptr_of(name)), "name_ptr");
    check_status(name_len_builder.Append(encode_len(name)), "name_len");
    check_status(uid_builder.Append(uid), "uid append");
    // memcpy the variant into a fixed-width byte buffer (no reinterpret_cast).
    std::array<char, kContextWidth> ctx_bytes {};
    copy_bytes(ctx_bytes.data(), &context, sizeof(context));
    check_status(
        ctx_builder.Append(std::string_view(ctx_bytes.data(), kContextWidth)),
        "context append");
  };

  for (const auto& c : meta.col_labels) {  // kind 0 = column
    append(0, c.class_name, c.variable_name, c.variable_uid, c.context);
  }
  for (const auto& r : meta.row_labels) {  // kind 1 = row
    append(1, r.class_name, r.constraint_name, r.variable_uid, r.context);
  }

  std::shared_ptr<arrow::Array> kind_array;
  std::shared_ptr<arrow::Array> class_ptr_array;
  std::shared_ptr<arrow::Array> class_len_array;
  std::shared_ptr<arrow::Array> name_ptr_array;
  std::shared_ptr<arrow::Array> name_len_array;
  std::shared_ptr<arrow::Array> uid_array;
  std::shared_ptr<arrow::Array> ctx_array;
  check_status(kind_builder.Finish(&kind_array), "kind finish");
  check_status(class_ptr_builder.Finish(&class_ptr_array), "class_ptr finish");
  check_status(class_len_builder.Finish(&class_len_array), "class_len finish");
  check_status(name_ptr_builder.Finish(&name_ptr_array), "name_ptr finish");
  check_status(name_len_builder.Finish(&name_len_array), "name_len finish");
  check_status(uid_builder.Finish(&uid_array), "uid finish");
  check_status(ctx_builder.Finish(&ctx_array), "context finish");

  const auto schema = arrow::schema({
      arrow::field("kind", arrow::int8()),
      arrow::field("class_ptr", arrow::int64()),
      arrow::field("class_len", arrow::uint8()),
      arrow::field("name_ptr", arrow::int64()),
      arrow::field("name_len", arrow::uint8()),
      arrow::field("uid", arrow::int32()),
      arrow::field("context", arrow::fixed_size_binary(kContextWidth)),
  });
  return arrow::Table::Make(schema,
                            {
                                kind_array,
                                class_ptr_array,
                                class_len_array,
                                name_ptr_array,
                                name_len_array,
                                uid_array,
                                ctx_array,
                            });
}

/// Write `table` to a Parquet file at `path`, tuned for fast read/write of a
/// throwaway temp file (same recipe as the SDDP cut writer):
///   * per-column min/max statistics disabled — never read back here;
///   * page index disabled — trims the footer;
///   * Arrow schema metadata NOT stored — the native types round-trip;
///   * zstd codec — the decomposed pointer / context columns are highly
///     redundant (few distinct class pointers), so a fast codec shrinks the
///     spilled bytes for free.
void write_table(const std::filesystem::path& path,
                 const std::shared_ptr<arrow::Table>& table)
{
  auto open_result = arrow::io::FileOutputStream::Open(path.string());
  if (!open_result.ok()) {
    throw std::runtime_error("open " + path.string() + ": "
                             + open_result.status().ToString());
  }
  const auto& out = *open_result;

  parquet::WriterProperties::Builder props_builder;
  props_builder.compression(parquet::Compression::ZSTD);
  props_builder.disable_statistics();
  props_builder.disable_write_page_index();
  const auto props = props_builder.build();

  check_status(parquet::arrow::WriteTable(*table,
                                          arrow::default_memory_pool(),
                                          out,
                                          /*chunk_size=*/1024UL * 1024UL,
                                          props),
               "write table");
  check_status(out->Close(), "close");
}

/// Split a long-format label-metadata table back into col/row label vectors.
[[nodiscard]] LpLabelMeta table_to_meta(const arrow::Table& table)
{
  const auto combined_result = table.CombineChunks();
  if (!combined_result.ok()) {
    throw std::runtime_error("combine chunks: "
                             + combined_result.status().ToString());
  }
  const auto& combined = *combined_result;

  LpLabelMeta out;
  const auto nrows = combined->num_rows();
  if (nrows == 0) {
    return out;  // empty set
  }

  const auto col = [&](const char* name)
  { return combined->GetColumnByName(name); };
  const auto kind_col = col("kind");
  const auto class_ptr_col = col("class_ptr");
  const auto class_len_col = col("class_len");
  const auto name_ptr_col = col("name_ptr");
  const auto name_len_col = col("name_len");
  const auto uid_col = col("uid");
  const auto ctx_col = col("context");
  if (!kind_col || !class_ptr_col || !class_len_col || !name_ptr_col
      || !name_len_col || !uid_col || !ctx_col)
  {
    throw std::runtime_error("lp_name_spill: missing metadata column");
  }

  const auto kinds =
      std::static_pointer_cast<arrow::Int8Array>(kind_col->chunk(0));
  const auto class_ptrs =
      std::static_pointer_cast<arrow::Int64Array>(class_ptr_col->chunk(0));
  const auto class_lens =
      std::static_pointer_cast<arrow::UInt8Array>(class_len_col->chunk(0));
  const auto name_ptrs =
      std::static_pointer_cast<arrow::Int64Array>(name_ptr_col->chunk(0));
  const auto name_lens =
      std::static_pointer_cast<arrow::UInt8Array>(name_len_col->chunk(0));
  const auto uids =
      std::static_pointer_cast<arrow::Int32Array>(uid_col->chunk(0));
  const auto ctxs =
      std::static_pointer_cast<arrow::FixedSizeBinaryArray>(ctx_col->chunk(0));

  for (std::int64_t i = 0; i < nrows; ++i) {
    const auto class_sv = view_of(class_ptrs->Value(i), class_lens->Value(i));
    const auto name_sv = view_of(name_ptrs->Value(i), name_lens->Value(i));
    const auto uid = Uid {uids->Value(i)};
    LpContext context {};
    copy_bytes(&context, ctxs->GetValue(i), sizeof(context));

    if (kinds->Value(i) == 0) {
      out.col_labels.push_back(SparseColLabel {
          .class_name = class_sv,
          .variable_name = name_sv,
          .variable_uid = uid,
          .context = context,
      });
    } else {
      out.row_labels.push_back(SparseRowLabel {
          .class_name = class_sv,
          .constraint_name = name_sv,
          .variable_uid = uid,
          .context = context,
      });
    }
  }
  return out;
}

}  // namespace

LpNameSpillStore::LpNameSpillStore(std::filesystem::path dir)
    : m_dir_(std::move(dir))
{
  std::error_code ec;
  std::filesystem::create_directories(m_dir_, ec);

  m_worker_ =
      std::jthread([this](const std::stop_token& stop) { worker_loop(stop); });
}

LpNameSpillStore::~LpNameSpillStore()
{
  m_worker_.request_stop();
  m_cv_.notify_all();
  if (m_worker_.joinable()) {
    m_worker_.join();  // no in-flight write survives this point
  }
  std::error_code ec;
  std::filesystem::remove_all(m_dir_, ec);
}

std::filesystem::path LpNameSpillStore::stem_for(const std::string& key) const
{
  return m_dir_ / key;  // parquet_read_table appends ".parquet"
}

void LpNameSpillStore::spill(std::string key, LpLabelMeta meta)
{
  {
    const std::scoped_lock lock(m_mtx_);
    m_pending_.insert(key);
    m_known_.insert(key);
    m_queue_.emplace_back(std::move(key), std::move(meta));
  }
  m_cv_.notify_all();
}

void LpNameSpillStore::worker_loop(const std::stop_token& stop)
{
  while (true) {
    std::pair<std::string, LpLabelMeta> task;
    {
      std::unique_lock lock(m_mtx_);
      m_cv_.wait(lock,
                 [&] { return !m_queue_.empty() || stop.stop_requested(); });
      if (m_queue_.empty()) {
        return;  // stop requested and nothing left to flush
      }
      task = std::move(m_queue_.front());
      m_queue_.pop_front();
    }

    // Heavy work (build Arrow table + Parquet encode + disk write) OFF the
    // lock.
    try {
      const auto table = meta_to_table(task.second);
      write_table(stem_for(task.first).replace_extension(".parquet"), table);
    } catch (const std::exception& e) {
      spdlog::warn("lp_name_spill: failed to write metadata for '{}': {}",
                   task.first,
                   e.what());
    }

    {
      const std::scoped_lock lock(m_mtx_);
      m_pending_.erase(task.first);
    }
    m_cv_.notify_all();
  }
}

const LpLabelMeta* LpNameSpillStore::load(const std::string& key)
{
  std::unique_lock lock(m_mtx_);
  if (const auto it = m_cache_.find(key); it != m_cache_.end()) {
    return &it->second;
  }
  if (!m_known_.contains(key)) {
    return nullptr;  // never spilled
  }
  // Wait for the worker to flush this key to disk, then read it back.
  m_cv_.wait(lock, [&] { return !m_pending_.contains(key); });

  LpLabelMeta meta;
  try {
    auto table_result = parquet_read_table(stem_for(key));
    if (!table_result.has_value()) {
      throw std::runtime_error(table_result.error());
    }
    meta = table_to_meta(**table_result);
  } catch (const std::exception& e) {
    spdlog::warn(
        "lp_name_spill: failed to load metadata for '{}': {}", key, e.what());
    return nullptr;
  }
  const auto [it, _] = m_cache_.emplace(key, std::move(meta));
  return &it->second;
}

void LpNameSpillStore::drain()
{
  std::unique_lock lock(m_mtx_);
  m_cv_.wait(lock, [&] { return m_pending_.empty(); });
}

}  // namespace gtopt
