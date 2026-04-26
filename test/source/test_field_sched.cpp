// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_field_sched.cpp
 * @brief     Unit tests for FieldSched variant and related aliases
 * @date      2026-04-23
 * @author    copilot
 * @copyright BSD-3-Clause
 */

#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/field_sched.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("RealFieldSched holds a scalar Real")  // NOLINT
{
  const RealFieldSched fs = 3.14;
  REQUIRE(std::holds_alternative<Real>(fs));
  CHECK(std::get<Real>(fs) == doctest::Approx(3.14));
}

TEST_CASE("RealFieldSched holds a vector of Real")  // NOLINT
{
  const std::vector<Real> v {1.0, 2.0, 3.0};
  const RealFieldSched fs = v;
  REQUIRE(std::holds_alternative<std::vector<Real>>(fs));
  CHECK(std::get<std::vector<Real>>(fs).size() == 3);
  CHECK(std::get<std::vector<Real>>(fs)[1] == doctest::Approx(2.0));
}

TEST_CASE("RealFieldSched holds a FileSched string")  // NOLINT
{
  const FileSched fname = "input/data.parquet";
  const RealFieldSched fs = fname;
  REQUIRE(std::holds_alternative<FileSched>(fs));
  CHECK(std::get<FileSched>(fs) == "input/data.parquet");
}

TEST_CASE("OptRealFieldSched default is nullopt")  // NOLINT
{
  const OptRealFieldSched opt {};
  CHECK_FALSE(opt.has_value());
}

TEST_CASE("OptRealFieldSched holds a scalar")  // NOLINT
{
  const OptRealFieldSched opt = RealFieldSched {42.0};
  REQUIRE(opt.has_value());
  CHECK(std::get<Real>(*opt) == doctest::Approx(42.0));
}

TEST_CASE("IntFieldSched holds a scalar Int")  // NOLINT
{
  const IntFieldSched fs = Int {7};
  REQUIRE(std::holds_alternative<Int>(fs));
  CHECK(std::get<Int>(fs) == 7);
}

TEST_CASE("IntFieldSched holds a vector of Int")  // NOLINT
{
  const std::vector<Int> v {10, 20, 30};
  const IntFieldSched fs = v;
  REQUIRE(std::holds_alternative<std::vector<Int>>(fs));
  CHECK(std::get<std::vector<Int>>(fs).size() == 3);
}

TEST_CASE("BoolFieldSched holds a Bool scalar")  // NOLINT
{
  const BoolFieldSched fs = Bool {true};
  REQUIRE(std::holds_alternative<Bool>(fs));
  CHECK(std::get<Bool>(fs) == true);
}

TEST_CASE("Active alias — scalar IntBool")  // NOLINT
{
  const Active a = IntBool {1};
  REQUIRE(std::holds_alternative<IntBool>(a));
  CHECK(std::get<IntBool>(a) == 1);
}

TEST_CASE("Active alias — vector of IntBool")  // NOLINT
{
  const std::vector<IntBool> v {1, 0, 1, 1};
  const Active a = v;
  REQUIRE(std::holds_alternative<std::vector<IntBool>>(a));
  CHECK(std::get<std::vector<IntBool>>(a).size() == 4);
}

TEST_CASE("Active alias — FileSched")  // NOLINT
{
  const Active a = FileSched {"active_schedule.csv"};
  REQUIRE(std::holds_alternative<FileSched>(a));
  CHECK(std::get<FileSched>(a) == "active_schedule.csv");
}

TEST_CASE("OptActive default is nullopt")  // NOLINT
{
  const OptActive oa {};
  CHECK_FALSE(oa.has_value());
}

TEST_CASE("OptActive with scalar value")  // NOLINT
{
  const OptActive oa = Active {IntBool {0}};
  REQUIRE(oa.has_value());
  CHECK(std::get<IntBool>(*oa) == 0);
}

TEST_CASE("TRealFieldSched is an alias for RealFieldSched")  // NOLINT
{
  static_assert(std::is_same_v<TRealFieldSched, RealFieldSched>);
  const TRealFieldSched fs = 99.0;
  CHECK(std::get<Real>(fs) == doctest::Approx(99.0));
}

TEST_CASE("FieldSched2 inner vector variant")  // NOLINT
{
  using V2 = std::vector<std::vector<Real>>;
  const V2 data {{1.0, 2.0}, {3.0, 4.0}};
  const RealFieldSched2 fs = data;
  REQUIRE(std::holds_alternative<V2>(fs));
  CHECK(std::get<V2>(fs).size() == 2);
  CHECK(std::get<V2>(fs)[0][1] == doctest::Approx(2.0));
}
