/**
 * @file      basis_io.hpp
 * @brief     Binary (de)serialization of a solver-agnostic simplex `Basis`
 * @date      2026-07-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A `Basis` (`basis.hpp`) is two `uint8_t` status vectors (columns + rows);
 * these free functions round-trip it to a small binary file so a warm-start
 * run can skip a cold barrier+crossover on the (identical) root LP relaxation.
 *
 * On-disk layout (little-endian, tightly packed):
 *
 *   magic     : 4 bytes  "GBAS"      (Gtopt BASis)
 *   version   : uint32   currently 1
 *   num_cols  : uint64   == Basis::col_status.size()
 *   num_rows  : uint64   == Basis::row_status.size()
 *   col_status: num_cols bytes (one BasisStatus per column)
 *   row_status: num_rows bytes (one BasisStatus per row)
 *
 * Every failure path (missing file, bad magic/version, truncated body,
 * out-of-range status byte) returns an `Error` — the functions never throw
 * and never crash on a garbage file.
 */

#pragma once

#include <expected>
#include <filesystem>

#include <gtopt/basis.hpp>
#include <gtopt/error.hpp>

namespace gtopt
{

/// Serialize `basis` to `path` in the binary layout documented above.
/// @return empty on success, or a `FileIOError` if the file cannot be written.
[[nodiscard]] std::expected<void, Error> save_basis(
    const std::filesystem::path& path, const Basis& basis);

/// Deserialize a `Basis` from `path`.
/// @return the reconstructed basis, or a `FileIOError` (file missing /
///         unreadable) / `InvalidInput` (bad magic, version, or truncated /
///         corrupt body) — never throws.
[[nodiscard]] std::expected<Basis, Error> load_basis(
    const std::filesystem::path& path);

}  // namespace gtopt
