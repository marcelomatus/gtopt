#include <doctest/doctest.h>
#include <gtopt/as_label.hpp>

using namespace gtopt;

TEST_CASE("as_label basic functionality")
{
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
    CHECK(as_label<' '>("Hello", "world") == "hello world");
    CHECK(as_label<','>(1, 2, 3) == "1,2,3");
  }

  SUBCASE("mixed types")
  {
    const std::string s = "str";
    const std::string_view sv = "view";
    CHECK(as_label(s, sv, 42) == "str_view_42");
    CHECK(as_label("prefix", 3.14, "suffix") == "prefix_3.14_suffix");
    CHECK(as_label("prefiX", 3.14, "Suffix") == "prefix_3.14_suffix");
  }

  SUBCASE("edge cases")
  {
    CHECK(as_label("", "b", "") == "b");
    CHECK(as_label("a", "", "c") == "a_c");
    CHECK(as_label("", "", "") == "");
  }
}
