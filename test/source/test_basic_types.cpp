#include <string>
#include <utility>

#include <doctest/doctest.h>
#include <gtopt/as_label.hpp>
#include <gtopt/basic_types.hpp>
#include <gtopt/error.hpp>
#include <gtopt/utils.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

static_assert(gtopt::detail::to_lower_char('A') == 'a');

TEST_CASE("Name")
{
  using namespace gtopt;

  const Name name = "bus_1";
  const Uid uid {1};

  CHECK(name == "bus_1");
  CHECK(uid == 1);
}

TEST_CASE("gtopt types 1")  // NOLINT
{
  REQUIRE(gtopt::as_label("") == "");
  REQUIRE(gtopt::as_label("s1") == "s1");
  REQUIRE(gtopt::as_label("s1", "s2") == "s1_s2");
  REQUIRE(gtopt::as_label("s1", 1, 2) == "s1_1_2");

  REQUIRE(gtopt::as_label("s1", 1) == "s1_1");
  REQUIRE(gtopt::as_label("s1") == "s1");
  REQUIRE(gtopt::as_label("").empty());
  REQUIRE(gtopt::as_label().empty());

  const std::string s2 = "s2";
  REQUIRE(gtopt::as_label("s1", s2) == "s1_s2");
}

template<typename IndexType = size_t, typename Range>
inline auto my_enumerate(const Range& range)  // NOLINT
{
  return std::ranges::views::zip(
      std::ranges::views::iota(0)
          | std::ranges::views::transform(
              [](size_t i) { return static_cast<IndexType>(i); }),
      range);
}

TEST_CASE("gtopt types 2")  // NOLINT
{
  const std::vector<int> vec1 = {3, 4, 5, 6, 7};
  const std::vector<int> vec2 = {1, 2, 3, 4, 5};

  auto rng = std::ranges::views::zip(vec1, vec2);

  for (size_t i = 0; auto&& [v1, v2] : rng) {
    REQUIRE(v1 == vec1[i]);
    REQUIRE(v2 == vec2[i]);
    ++i;
  }

  auto rng2 = std::ranges::views::zip(
      std::ranges::views::iota(0)
          | std::ranges::views::transform([](size_t i) { return i; }),
      vec1);
  for (auto&& [i, v1] : rng2) {
    REQUIRE(v1 == vec1[i]);
  }

  for (auto&& [i, v1] : my_enumerate(vec1)) {
    REQUIRE(v1 == vec1[i]);
  }
}

TEST_CASE("as_label basic functionality")
{
  using namespace gtopt;

  SUBCASE("empty input")
  {
    CHECK(as_label() == "");
    CHECK(as_label("") == "");
    CHECK(as_label("", "") == "");
  }

  SUBCASE("single argument")
  {
    CHECK(as_label("hello") == "hello");
    CHECK(as_label(42) == "42");
    CHECK(as_label(3.14) == "3.14");
  }

  SUBCASE("multiple arguments")
  {
    CHECK(as_label("hello", "world") == "hello_world");
    CHECK(as_label("test", 1, 2) == "test_1_2");
    CHECK(as_label(1, 2, 3) == "1_2_3");
  }

  SUBCASE("custom separator")
  {
    CHECK(as_label<'-'>("a", "b", "c") == "a-b-c");
    CHECK(as_label<' '>("Hello", "world") == "Hello world");
    CHECK(as_label<','>(1, 2, 3) == "1,2,3");
    CHECK(as_label<'X'>("A", "B") == "AXB");
  }

  SUBCASE("mixed types")
  {
    const std::string s = "str";
    const std::string_view sv = "view";
    CHECK(as_label(s, sv, 42) == "str_view_42");
    CHECK(as_label("prefix", 3.14, "suffix") == "prefix_3.14_suffix");
    CHECK(as_label("prefiX", 3.14, "Suffix") == "prefiX_3.14_Suffix");
  }

  SUBCASE("edge cases")
  {
    CHECK(as_label("", "b", "") == "b");
    CHECK(as_label("a", "", "c") == "a_c");
    CHECK(as_label("", "", "") == "");
  }

  SUBCASE("numeric edge cases")
  {
    CHECK(as_label(0) == "0");
    CHECK(as_label(-1) == "-1");
    CHECK(as_label(0.0) == "0");
  }

  SUBCASE("case preserved")
  {
    CHECK(as_label("ABC") == "ABC");
    CHECK(as_label("Hello World") == "Hello World");
    CHECK(as_label("MiXeD") == "MiXeD");
  }

  SUBCASE("lowercase helper")
  {
    CHECK(lowercase("ABC") == "abc");
    CHECK(lowercase(as_label("Hello", "World")) == "hello_world");
    CHECK(lowercase(as_label("MiXeD")) == "mixed");
    CHECK(lowercase(std::string_view {"Generator"}) == "generator");
  }

  SUBCASE("long label")
  {
    CHECK(as_label("a", "b", "c", "d", "e") == "a_b_c_d_e");
  }
}

TEST_SUITE("Error")
{
  TEST_CASE("ErrorCode default values")
  {
    using namespace gtopt;

    CHECK(std::to_underlying(ErrorCode::Success) == 0);
    CHECK(std::to_underlying(ErrorCode::SolverError) == 1);
    CHECK(std::to_underlying(ErrorCode::InternalError) == 2);
    CHECK(std::to_underlying(ErrorCode::InvalidInput) == 3);
    CHECK(std::to_underlying(ErrorCode::FileIOError) == 4);
  }

  TEST_CASE("Error default construction")
  {
    using namespace gtopt;

    const Error err {};
    CHECK(err.code == ErrorCode::Success);
    CHECK(err.message.empty());
    CHECK(err.status == 0);
  }

  TEST_CASE("Error with values")
  {
    using namespace gtopt;

    const Error err {
        .code = ErrorCode::SolverError,
        .message = "solver failed",
        .status = -1,
    };

    CHECK(err.code == ErrorCode::SolverError);
    CHECK(err.message == "solver failed");
    CHECK(err.status == -1);
  }

  TEST_CASE("Error copy semantics")
  {
    using namespace gtopt;

    const Error err1 {
        .code = ErrorCode::InvalidInput,
        .message = "bad input",
        .status = 42,
    };

    const Error err2 =  // NOLINT(performance-unnecessary-copy-initialization)
        err1;
    CHECK(err2.code == ErrorCode::InvalidInput);
    CHECK(err2.message == "bad input");
    CHECK(err2.status == 42);
  }

  TEST_CASE("Error is_success and is_error")
  {
    using namespace gtopt;

    const Error success_err {};
    CHECK(success_err.is_success());
    CHECK_FALSE(success_err.is_error());

    const Error solver_err {
        .code = ErrorCode::SolverError,
        .message = "failed",
    };
    CHECK_FALSE(solver_err.is_success());
    CHECK(solver_err.is_error());

    const Error io_err {
        .code = ErrorCode::FileIOError,
        .message = "disk full",
        .status = -1,
    };
    CHECK_FALSE(io_err.is_success());
    CHECK(io_err.is_error());
  }
}
