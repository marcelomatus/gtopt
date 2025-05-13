#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/utils.hpp>

using namespace gtopt;

TEST_CASE("merge vectors")
{
  SUBCASE("basic merge")
  {
    std::vector<int> a {1, 2};
    std::vector<int> b {3, 4};
    merge(a, std::move(b));
    CHECK(a == std::vector {1, 2, 3, 4});
    CHECK(b.empty());
  }

  SUBCASE("empty source")
  {
    std::vector<int> a {1, 2};
    std::vector<int> b;
    merge(a, std::move(b));
    CHECK(a == std::vector {1, 2});
  }

  SUBCASE("empty destination")
  {
    std::vector<int> a;
    std::vector<int> b {1, 2};
    merge(a, std::move(b));
    CHECK(a == std::vector {1, 2});
  }

  SUBCASE("self merge")
  {
    std::vector<int> a {1, 2};
    merge(a, a);
    CHECK(a == std::vector {1, 2, 1, 2});
  }
}

TEST_CASE("enumerate")
{
  std::vector<std::string> vec {"a", "b", "c"};

  SUBCASE("basic enumeration")
  {
    size_t i = 0;
    for (auto [idx, val] : enumerate(vec)) {
      CHECK(idx == i++);
      CHECK(val == vec[idx]);
    }
  }

  SUBCASE("enumeration with custom index")
  {
    for (auto [idx, val] : enumerate<std::int16_t>(vec)) {
      static_assert(std::is_same_v<decltype(idx), std::int16_t>);
    }
  }
}

TEST_CASE("enumerate_if and active")
{
  struct TestElement
  {
    bool active;
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }
  };

  std::vector<TestElement> elements {{true}, {false}, {true}, {false}, {true}};

  SUBCASE("enumerate_active")
  {
    size_t count = 0;
    for (auto [idx, elem] : enumerate_active(elements)) {
      CHECK(elem.is_active());
      count++;
    }
    CHECK(count == 3);
  }

  SUBCASE("active view")
  {
    auto active_view = active(elements);
    CHECK(ranges::distance(active_view) == 3);
    for (const auto& elem : active_view) {
      CHECK(elem.is_active());
    }
  }
}

TEST_CASE("map utilities")
{
  std::map<int, std::string> test_map {{1, "one"}, {2, "two"}, {3, "three"}};

  SUBCASE("get_optiter")
  {
    auto opt = get_optiter(test_map, 2);
    REQUIRE(opt.has_value());
    CHECK(opt.value()->second == "two");

    auto opt2 = get_optiter(test_map, 42);
    CHECK_FALSE(opt2.has_value());
  }

  SUBCASE("get_optvalue")
  {
    auto opt = get_optvalue(test_map, 3);
    REQUIRE(opt.has_value());
    CHECK(*opt == "three");

    auto opt2 = get_optvalue(test_map, 42);
    CHECK_FALSE(opt2.has_value());
  }

  SUBCASE("get_optvalue_optkey")
  {
    auto opt = get_optvalue_optkey(test_map, std::optional<int> {2});
    REQUIRE(opt.has_value());
    CHECK(*opt == "two");

    auto opt2 = get_optvalue_optkey(test_map, std::optional<int> {});
    CHECK_FALSE(opt2.has_value());
  }
}

TEST_CASE("optional utilities")
{
  SUBCASE("merge_opt")
  {
    std::optional<int> a = 1;
    std::optional<int> b = 2;
    merge_opt(a, b);
    CHECK(a == 2);

    std::optional<int> c;
    merge_opt(a, c);
    CHECK(a == 2);

    merge_opt(a, std::optional<int> {3});
    CHECK(a == 3);
  }

  SUBCASE("has_value_fnc")
  {
    CHECK(has_value_fnc(std::optional<int> {42}));
    CHECK_FALSE(has_value_fnc(std::optional<int> {}));
  }

  SUBCASE("is_true_fnc")
  {
    CHECK(is_true_fnc(std::optional<bool> {true}));
    CHECK_FALSE(is_true_fnc(std::optional<bool> {false}));
    CHECK_FALSE(is_true_fnc(std::optional<bool> {}));
  }
}

TEST_CASE("to_vector")
{
  std::vector<int> original {1, 2, 3, 4, 5};

  SUBCASE("basic conversion")
  {
    auto vec = to_vector(original);
    CHECK(vec == original);
  }

  SUBCASE("with transform")
  {
    auto vec = to_vector(original, [](int x) { return x * 2; });
    CHECK(vec == std::vector {2, 4, 6, 8, 10});
  }
}

TEST_CASE("all_of")
{
  std::vector<int> nums {2, 4, 6, 8};

  SUBCASE("all even")
  {
    CHECK(all_of(nums, [](int x) { return x % 2 == 0; }));
  }

  SUBCASE("not all even")
  {
    nums.push_back(1);
    CHECK_FALSE(all_of(nums, [](int x) { return x % 2 == 0; }));
  }
}
