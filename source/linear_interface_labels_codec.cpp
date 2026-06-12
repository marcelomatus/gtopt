/**
 * @file      linear_interface_labels_codec.cpp
 * @brief     Byte-level (de)serializer for LP column / row label metadata.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include <gtopt/linear_interface_labels_codec.hpp>

namespace gtopt
{

namespace
{

/// Generic byte-level serializer.  Writes the same record layout for
/// both `SparseColLabel` and `SparseRowLabel`; the `if constexpr` branch
/// picks the `variable_name` vs `constraint_name` field based on the
/// label type.
template<typename LabelT>
[[nodiscard]] std::vector<char> serialize_labels_meta_impl(
    const std::vector<LabelT>& labels)
{
  std::vector<char> out;
  // Pre-reserve a conservative estimate (2 strings × 16 + uid +
  // context).  Realloc paths are fine but this avoids most.
  out.reserve(labels.size() * 80);

  const auto write_u16 = [&](uint16_t v)
  {
    // Promote to unsigned int explicitly so the right-shift is in
    // unsigned arithmetic (avoids hicpp-signed-bitwise).
    const auto vu = static_cast<unsigned>(v);
    out.push_back(static_cast<char>(vu & 0xFFU));
    out.push_back(static_cast<char>((vu >> 8U) & 0xFFU));
  };
  const auto write_sv = [&](std::string_view s)
  {
    assert(s.size() <= std::numeric_limits<uint16_t>::max()
           && "LP class/variable/constraint name too long for u16 length");
    write_u16(static_cast<uint16_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
  };
  const auto write_bytes = [&](const void* ptr, std::size_t n)
  {
    // Express the pointer + length as a span and iterate it instead of
    // raw `p + n` pointer arithmetic.
    const std::span<const char> bytes {static_cast<const char*>(ptr), n};
    out.insert(out.end(), bytes.begin(), bytes.end());
  };

  for (const auto& lbl : labels) {
    write_sv(lbl.class_name);
    if constexpr (requires { lbl.variable_name; }) {
      write_sv(lbl.variable_name);
    } else {
      write_sv(lbl.constraint_name);
    }
    write_bytes(&lbl.variable_uid, sizeof(lbl.variable_uid));
    write_bytes(&lbl.context, sizeof(lbl.context));
  }
  return out;
}

/// Generic byte-level deserializer; re-materialises each string into
/// `string_pool` so the revived string_views point at stable storage.
template<typename LabelT>
[[nodiscard]] std::vector<LabelT> deserialize_labels_meta_impl(
    std::span<const char> data,
    std::size_t num_entries,
    std::vector<std::string>& string_pool)
{
  std::vector<LabelT> out;
  out.reserve(num_entries);

  std::size_t pos = 0;
  const auto read_u16 = [&]() -> uint16_t
  {
    // unsigned int locals so the left-shift stays in unsigned space
    // (avoids hicpp-signed-bitwise on the implicit-promotion-to-int).
    const unsigned lo = static_cast<uint8_t>(data[pos]);
    const unsigned hi = static_cast<uint8_t>(data[pos + 1]);
    pos += 2;
    return static_cast<uint16_t>(lo | (hi << 8U));
  };
  const auto read_sv = [&]() -> std::string_view
  {
    const auto n = read_u16();
    const auto bytes = data.subspan(pos, n);
    string_pool.emplace_back(bytes.data(), bytes.size());
    pos += n;
    return string_pool.back();
  };
  const auto read_bytes = [&](void* ptr, std::size_t n)
  {
    const auto bytes = data.subspan(pos, n);
    std::memcpy(ptr, bytes.data(), bytes.size());
    pos += n;
  };

  for (std::size_t k = 0; k < num_entries; ++k) {
    LabelT lbl {};
    lbl.class_name = read_sv();
    if constexpr (requires { lbl.variable_name; }) {
      lbl.variable_name = read_sv();
    } else {
      lbl.constraint_name = read_sv();
    }
    read_bytes(&lbl.variable_uid, sizeof(lbl.variable_uid));
    read_bytes(&lbl.context, sizeof(lbl.context));
    out.push_back(std::move(lbl));
  }
  return out;
}

}  // namespace

std::vector<char> serialize_labels_meta(
    const std::vector<SparseColLabel>& labels)
{
  return serialize_labels_meta_impl(labels);
}

std::vector<char> serialize_labels_meta(
    const std::vector<SparseRowLabel>& labels)
{
  return serialize_labels_meta_impl(labels);
}

std::vector<SparseColLabel> deserialize_col_labels_meta(
    std::span<const char> data,
    std::size_t num_entries,
    std::vector<std::string>& string_pool)
{
  return deserialize_labels_meta_impl<SparseColLabel>(
      data, num_entries, string_pool);
}

std::vector<SparseRowLabel> deserialize_row_labels_meta(
    std::span<const char> data,
    std::size_t num_entries,
    std::vector<std::string>& string_pool)
{
  return deserialize_labels_meta_impl<SparseRowLabel>(
      data, num_entries, string_pool);
}

}  // namespace gtopt
