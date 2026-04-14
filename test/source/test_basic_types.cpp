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

  // char arguments are treated as single characters, not integers
  REQUIRE(gtopt::as_label('x') == "x");
  REQUIRE(gtopt::as_label<','>('o', 999, 1, "name") == "o,999,1,name");
  REQUIRE(gtopt::as_label<'_'>('f', "tag") == "f_tag");

  // doubles: full precision, whole numbers get ".0" suffix
  REQUIRE(gtopt::as_label(1.0) == "1.0");
  REQUIRE(gtopt::as_label(0.0) == "0.0");
  REQUIRE(gtopt::as_label(-3.0) == "-3.0");
  REQUIRE(gtopt::as_label(1.5) == "1.5");
  REQUIRE(gtopt::as_label(3.14159265358979) == "3.14159265358979");
  REQUIRE(gtopt::as_label<','>('o', 999, 1.0, "name") == "o,999,1.0,name");
}

template<typename IndexType = size_t, typename Range>
inline auto my_enumerate(const Range& range)  // NOLINT
{
  return std::views::enumerate(range)
      | std::views::transform(
             [](auto&& pair)
             {
               return std::pair {static_cast<IndexType>(std::get<0>(pair)),
                                 std::get<1>(pair)};
             });
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

  for (auto&& [i, v1] : std::views::enumerate(vec1)) {
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
    CHECK(as_label(0.0) == "0.0");
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

  SUBCASE("lowercase view with as_label")
  {
    // lowercase(string_view) returns a LowercaseView — no allocation in
    // lowercase() itself; characters are lowercased when as_label copies them.
    const std::string_view gen = "Generator";
    CHECK(as_label(lowercase(gen), 1, 2) == "generator_1_2");
    CHECK(as_label(lowercase(gen)) == "generator");
    CHECK(as_label("Prefix", lowercase(gen), 42) == "Prefix_generator_42");

    // String literals also produce a LowercaseView (lazy view into static
    // storage)
    CHECK(as_label(lowercase("Battery"), 3) == "battery_3");

    // LowercaseView comparison directly
    CHECK(lowercase("ABC") == "abc");
    CHECK(lowercase(std::string_view {"MiXeD"}) == "mixed");

    // LowercaseView explicit conversion to string
    CHECK(std::string(lowercase("Hello")) == "hello");
  }

  SUBCASE("snake_case helper — eager std::string overload")
  {
    // Word boundaries around lower→upper and acronym+word transitions.
    CHECK(snake_case(std::string {"GeneratorLP"}) == "generator_lp");
    CHECK(snake_case(std::string {"HTTPResponse"}) == "http_response");
    CHECK(snake_case(std::string {"XMLHttpRequest"}) == "xml_http_request");
    CHECK(snake_case(std::string {"userID"}) == "user_id");
    CHECK(snake_case(std::string {"Gen2Bus"}) == "gen2_bus");
    // Already snake — unchanged.
    CHECK(snake_case(std::string {"already_snake"}) == "already_snake");
    CHECK(snake_case(std::string {"lower"}) == "lower");
    // All caps acronym.
    CHECK(snake_case(std::string {"HTTP"}) == "http");
    // Single character and empty cases.
    CHECK(snake_case(std::string {"A"}) == "a");
    CHECK(snake_case(std::string {""}).empty());
  }

  SUBCASE("snake_case view with as_label")
  {
    // snake_case(string-like) returns a SnakeCaseView — no allocation
    // until characters are copied out (e.g. by as_label or std::string()).
    const std::string_view cn = "GeneratorLP";
    CHECK(as_label(snake_case(cn), 1, 2) == "generator_lp_1_2");
    CHECK(as_label(snake_case(cn)) == "generator_lp");
    CHECK(as_label("Prefix", snake_case(cn), 42) == "Prefix_generator_lp_42");

    // String literals produce a SnakeCaseView (lazy view into static
    // storage)
    CHECK(as_label(snake_case("HTTPResponse"), 3) == "http_response_3");

    // SnakeCaseView comparison directly against string_view.
    CHECK(snake_case("GeneratorLP") == "generator_lp");
    CHECK(snake_case(std::string_view {"XMLHttpRequest"})
          == "xml_http_request");

    // Explicit conversion to std::string.
    CHECK(std::string(snake_case("userID")) == "user_id");

    // size() reports the exact output length — enabling as_label to
    // reserve before copying.
    CHECK(snake_case(std::string_view {"GeneratorLP"}).size()
          == std::string_view {"generator_lp"}.size());
    CHECK(snake_case(std::string_view {""}).empty());
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
