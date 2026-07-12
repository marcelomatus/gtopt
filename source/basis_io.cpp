/**
 * @file      basis_io.cpp
 * @brief     Binary (de)serialization of a solver-agnostic simplex `Basis`
 * @date      2026-07-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>

#include <gtopt/basis_io.hpp>

namespace gtopt
{

namespace
{
constexpr std::array<char, 4> kMagic {'G', 'B', 'A', 'S'};
constexpr uint32_t kVersion = 1;

/// The four defined `BasisStatus` values (`basis.hpp`).  Any byte outside this
/// range in a loaded file signals corruption.
constexpr uint8_t kMaxStatus = static_cast<uint8_t>(BasisStatus::free);

[[nodiscard]] Error file_error(std::string message)
{
  return Error {.code = ErrorCode::FileIOError, .message = std::move(message)};
}

[[nodiscard]] Error corrupt_error(std::string message)
{
  return Error {.code = ErrorCode::InvalidInput, .message = std::move(message)};
}

/// Append a fixed-width little-endian scalar to `buf` via `std::bit_cast`
/// (no `reinterpret_cast`).  `T` must be trivially copyable.
template<typename T>
void put_scalar(std::vector<char>& buf, const T& value)
{
  const auto bytes = std::bit_cast<std::array<char, sizeof(T)>>(value);
  buf.insert(buf.end(), bytes.begin(), bytes.end());
}

/// Read a fixed-width scalar from `src` at `off`, advancing `off`.  Returns
/// false if `src` does not have `sizeof(T)` bytes remaining.
template<typename T>
[[nodiscard]] bool get_scalar(std::span<const char> src,
                              std::size_t& off,
                              T& out)
{
  if (off + sizeof(T) > src.size()) {
    return false;
  }
  std::array<char, sizeof(T)> bytes {};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes[i] = src[off + i];
  }
  out = std::bit_cast<T>(bytes);
  off += sizeof(T);
  return true;
}
}  // namespace

std::expected<void, Error> save_basis(const std::filesystem::path& path,
                                      const Basis& basis)
{
  const uint64_t ncols = basis.col_status.size();
  const uint64_t nrows = basis.row_status.size();

  // Assemble the whole file in a byte buffer, then write once — no
  // `reinterpret_cast`, and status bytes copy 1:1 (BasisStatus is a uint8_t
  // enum, so its numeric value is the on-disk byte).
  std::vector<char> buf;
  buf.reserve(kMagic.size() + sizeof(kVersion) + (2 * sizeof(uint64_t))
              + static_cast<std::size_t>(ncols)
              + static_cast<std::size_t>(nrows));
  buf.insert(buf.end(), kMagic.begin(), kMagic.end());
  put_scalar(buf, kVersion);
  put_scalar(buf, ncols);
  put_scalar(buf, nrows);
  for (const auto s : basis.col_status) {
    buf.push_back(static_cast<char>(static_cast<uint8_t>(s)));
  }
  for (const auto s : basis.row_status) {
    buf.push_back(static_cast<char>(static_cast<uint8_t>(s)));
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return std::unexpected(file_error(std::format(
        "save_basis: cannot open '{}' for writing", path.string())));
  }
  out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  if (!out) {
    return std::unexpected(file_error(
        std::format("save_basis: write to '{}' failed", path.string())));
  }
  return {};
}

std::expected<Basis, Error> load_basis(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return std::unexpected(
        file_error(std::format("load_basis: cannot open '{}'", path.string())));
  }
  const std::streamsize size = in.tellg();
  if (size < 0) {
    return std::unexpected(
        file_error(std::format("load_basis: cannot size '{}'", path.string())));
  }
  in.seekg(0);
  std::vector<char> buf(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(buf.data(), size);
    if (!in || in.gcount() != size) {
      return std::unexpected(file_error(
          std::format("load_basis: read of '{}' failed", path.string())));
    }
  }
  const std::span<const char> src {buf};

  std::size_t off = 0;
  // Magic.
  if (src.size() < kMagic.size()
      || !std::equal(kMagic.begin(), kMagic.end(), src.begin()))
  {
    return std::unexpected(corrupt_error(
        std::format("load_basis: '{}' is not a gtopt basis file (bad magic)",
                    path.string())));
  }
  off += kMagic.size();

  uint32_t version = 0;
  if (!get_scalar(src, off, version) || version != kVersion) {
    return std::unexpected(corrupt_error(
        std::format("load_basis: '{}' unsupported/absent version (expected {})",
                    path.string(),
                    kVersion)));
  }

  uint64_t ncols = 0;
  uint64_t nrows = 0;
  if (!get_scalar(src, off, ncols) || !get_scalar(src, off, nrows)) {
    return std::unexpected(corrupt_error(
        std::format("load_basis: '{}' truncated header", path.string())));
  }

  // Body must hold exactly ncols + nrows status bytes.
  if (off + static_cast<std::size_t>(ncols) + static_cast<std::size_t>(nrows)
      > src.size())
  {
    return std::unexpected(corrupt_error(std::format(
        "load_basis: '{}' truncated body (expected {} cols + {} rows)",
        path.string(),
        ncols,
        nrows)));
  }

  Basis basis;
  basis.col_status.reserve(static_cast<std::size_t>(ncols));
  basis.row_status.reserve(static_cast<std::size_t>(nrows));
  const auto read_status = [&](std::vector<BasisStatus>& dst,
                               uint64_t n) -> bool
  {
    for (uint64_t i = 0; i < n; ++i) {
      const auto byte = static_cast<uint8_t>(src[off++]);
      if (byte > kMaxStatus) {
        return false;
      }
      dst.push_back(static_cast<BasisStatus>(byte));
    }
    return true;
  };
  if (!read_status(basis.col_status, ncols)
      || !read_status(basis.row_status, nrows))
  {
    return std::unexpected(corrupt_error(std::format(
        "load_basis: '{}' has an out-of-range status byte", path.string())));
  }

  return basis;
}

}  // namespace gtopt
