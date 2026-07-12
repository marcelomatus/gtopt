// SPDX-License-Identifier: BSD-3-Clause
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>
#include <gtopt/basis.hpp>
#include <gtopt/basis_io.hpp>

using namespace gtopt;

namespace
{
/// A unique temp path in the system temp dir, cleaned up by the caller.
std::filesystem::path temp_basis_path(const char* stem)
{
  return std::filesystem::temp_directory_path()
      / (std::string {"gtopt_basis_io_"} + stem + ".bin");
}
}  // namespace

TEST_CASE("basis_io round-trips a Basis exactly")  // NOLINT
{
  const auto path = temp_basis_path("roundtrip");
  std::filesystem::remove(path);

  Basis basis;
  basis.col_status = {
      BasisStatus::basic,
      BasisStatus::at_lower,
      BasisStatus::at_upper,
      BasisStatus::free,
      BasisStatus::basic,
  };
  basis.row_status = {
      BasisStatus::at_lower,
      BasisStatus::basic,
      BasisStatus::at_upper,
  };

  SUBCASE("save then load reproduces the vectors")
  {
    const auto saved = save_basis(path, basis);
    REQUIRE(saved.has_value());
    REQUIRE(std::filesystem::exists(path));

    const auto loaded = load_basis(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->num_cols() == basis.num_cols());
    CHECK(loaded->num_rows() == basis.num_rows());
    CHECK(loaded->col_status == basis.col_status);
    CHECK(loaded->row_status == basis.row_status);
  }

  std::filesystem::remove(path);
}

TEST_CASE("basis_io round-trips an empty Basis")  // NOLINT
{
  const auto path = temp_basis_path("empty");
  std::filesystem::remove(path);

  const Basis basis {};  // no columns, no rows
  REQUIRE(basis.empty());

  const auto saved = save_basis(path, basis);
  REQUIRE(saved.has_value());

  const auto loaded = load_basis(path);
  REQUIRE(loaded.has_value());
  CHECK(loaded->empty());
  CHECK(loaded->num_cols() == 0);
  CHECK(loaded->num_rows() == 0);

  std::filesystem::remove(path);
}

TEST_CASE("basis_io fails cleanly on a missing file")  // NOLINT
{
  const auto path = temp_basis_path("does_not_exist");
  std::filesystem::remove(path);
  REQUIRE(!std::filesystem::exists(path));

  const auto loaded = load_basis(path);
  REQUIRE(!loaded.has_value());
  CHECK(loaded.error().code == ErrorCode::FileIOError);
}

TEST_CASE("basis_io rejects a garbage / non-basis file")  // NOLINT
{
  const auto path = temp_basis_path("garbage");
  std::filesystem::remove(path);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "not a gtopt basis file at all";
  }

  const auto loaded = load_basis(path);
  REQUIRE(!loaded.has_value());
  // Bad magic → InvalidInput, never a crash.
  CHECK(loaded.error().code == ErrorCode::InvalidInput);

  std::filesystem::remove(path);
}

TEST_CASE("basis_io rejects a truncated body")  // NOLINT
{
  const auto path = temp_basis_path("truncated");
  std::filesystem::remove(path);

  // Write a valid basis, then truncate it mid-body so the declared lengths
  // exceed the bytes actually present.
  Basis basis;
  basis.col_status = {BasisStatus::basic, BasisStatus::at_lower};
  basis.row_status = {BasisStatus::basic, BasisStatus::at_upper};
  REQUIRE(save_basis(path, basis).has_value());

  const auto full_size = std::filesystem::file_size(path);
  REQUIRE(full_size > 1);
  std::filesystem::resize_file(path, full_size - 1);

  const auto loaded = load_basis(path);
  REQUIRE(!loaded.has_value());
  CHECK(loaded.error().code == ErrorCode::InvalidInput);

  std::filesystem::remove(path);
}

TEST_CASE("basis_io rejects an out-of-range status byte")  // NOLINT
{
  const auto path = temp_basis_path("badstatus");
  std::filesystem::remove(path);

  // Hand-craft a well-formed header (magic "GBAS", version 1, 1 col, 0 rows)
  // whose single status byte (0xFF) is not a defined BasisStatus.
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char magic[4] = {'G', 'B', 'A', 'S'};
    out.write(magic, 4);
    const uint32_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    const uint64_t ncols = 1;
    const uint64_t nrows = 0;
    out.write(reinterpret_cast<const char*>(&ncols), sizeof(ncols));
    out.write(reinterpret_cast<const char*>(&nrows), sizeof(nrows));
    const auto bad = static_cast<char>(0xFF);
    out.write(&bad, 1);
  }

  const auto loaded = load_basis(path);
  REQUIRE(!loaded.has_value());
  CHECK(loaded.error().code == ErrorCode::InvalidInput);

  std::filesystem::remove(path);
}
