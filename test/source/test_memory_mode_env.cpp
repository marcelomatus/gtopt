// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_memory_mode_env.cpp
 * @brief     GTOPT_MEMORY_MODE env override of
 * PlanningOptionsLP::sddp_low_memory
 */

#include <cstdlib>
#include <optional>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/planning_options_lp.hpp>

using namespace gtopt;

namespace
{

/// RAII: set (or clear) GTOPT_MEMORY_MODE for a scope, restoring the
/// PREVIOUS value on destruction so parallel/whole-binary runs that carry
/// the variable in the ambient environment are left untouched.
struct ScopedMemoryModeEnv
{
  explicit ScopedMemoryModeEnv(const char* value)
  {
    if (const char* const prev = std::getenv("GTOPT_MEMORY_MODE");
        prev != nullptr)
    {
      prev_ = prev;
    }
    if (value != nullptr) {
      ::setenv("GTOPT_MEMORY_MODE", value, 1);
    } else {
      ::unsetenv("GTOPT_MEMORY_MODE");
    }
  }

  ~ScopedMemoryModeEnv()
  {
    if (prev_.has_value()) {
      ::setenv("GTOPT_MEMORY_MODE", prev_->c_str(), 1);
    } else {
      ::unsetenv("GTOPT_MEMORY_MODE");
    }
  }

  ScopedMemoryModeEnv(const ScopedMemoryModeEnv&) = delete;
  ScopedMemoryModeEnv(ScopedMemoryModeEnv&&) = delete;
  ScopedMemoryModeEnv& operator=(const ScopedMemoryModeEnv&) = delete;
  ScopedMemoryModeEnv& operator=(ScopedMemoryModeEnv&&) = delete;

private:
  std::optional<std::string> prev_;
};

[[nodiscard]] auto opts_with_low_memory(LowMemoryMode mode) -> PlanningOptionsLP
{
  PlanningOptions p;
  p.sddp_options.low_memory_mode = mode;
  return PlanningOptionsLP {p};
}

}  // namespace

TEST_CASE("GTOPT_MEMORY_MODE env override of sddp_low_memory")  // NOLINT
{
  SUBCASE("unset → per-run option / default (compress)")
  {
    const ScopedMemoryModeEnv env {nullptr};
    const PlanningOptionsLP defaulted;  // low_memory_mode unset → compress
    CHECK(defaulted.sddp_low_memory() == LowMemoryMode::compress);
    CHECK(opts_with_low_memory(LowMemoryMode::off).sddp_low_memory()
          == LowMemoryMode::off);
  }

  SUBCASE("env=compress forces compress, overriding an explicit off")
  {
    const ScopedMemoryModeEnv env {"compress"};
    CHECK(opts_with_low_memory(LowMemoryMode::off).sddp_low_memory()
          == LowMemoryMode::compress);
  }

  SUBCASE("env=off forces off, overriding an explicit compress")
  {
    const ScopedMemoryModeEnv env {"off"};
    CHECK(opts_with_low_memory(LowMemoryMode::compress).sddp_low_memory()
          == LowMemoryMode::off);
  }

  SUBCASE("alias normal/uncompress → off (memory used, uncompressed)")
  {
    {
      const ScopedMemoryModeEnv env {"normal"};
      CHECK(opts_with_low_memory(LowMemoryMode::compress).sddp_low_memory()
            == LowMemoryMode::off);
    }
    {
      const ScopedMemoryModeEnv env {"uncompress"};
      CHECK(opts_with_low_memory(LowMemoryMode::compress).sddp_low_memory()
            == LowMemoryMode::off);
    }
  }

  SUBCASE("alias snapshot/rebuild → compress")
  {
    {
      const ScopedMemoryModeEnv env {"snapshot"};
      CHECK(opts_with_low_memory(LowMemoryMode::off).sddp_low_memory()
            == LowMemoryMode::compress);
    }
    {
      const ScopedMemoryModeEnv env {"rebuild"};
      CHECK(opts_with_low_memory(LowMemoryMode::off).sddp_low_memory()
            == LowMemoryMode::compress);
    }
  }

  SUBCASE("unrecognised value → ignored, falls back to option")
  {
    const ScopedMemoryModeEnv env {"bogus"};
    CHECK(opts_with_low_memory(LowMemoryMode::off).sddp_low_memory()
          == LowMemoryMode::off);
  }
}
