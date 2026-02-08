#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
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
    CHECK(b.empty());  // NOLINT
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
    CHECK(a == std::vector {1, 2});
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
    CHECK(std::ranges::distance(active_view) == 3);
    for (const auto& elem : active_view) {
      CHECK(elem.is_active());
    }
  }
}

TEST_CASE("map utilities")
{
  const std::map<int, std::string> test_map {
      {1, "one"},
      {2, "two"},
      {3, "three"},
  };

  SUBCASE("get_optiter")
  {
    auto opt = get_optiter(test_map, 2);
    REQUIRE(opt);
    CHECK((*opt)->first == 2);
    CHECK((*opt)->second == "two");

    auto opt2 = get_optiter(test_map, 42);
    CHECK_FALSE(opt2);
  }

  SUBCASE("get_optvalue")
  {
    auto opt = get_optvalue(test_map, 3);
    REQUIRE(opt);
    CHECK(*opt == "three");

    auto opt2 = get_optvalue(test_map, 42);
    CHECK_FALSE(opt2);
  }

  SUBCASE("get_optvalue_optkey")
  {
    auto opt = get_optvalue_optkey(test_map, std::optional<int> {2});
    REQUIRE(opt);
    CHECK(*opt == "two");

    auto opt2 = get_optvalue_optkey(test_map, std::optional<int> {});
    CHECK_FALSE(opt2);

    auto opt3 = get_optvalue_optkey(test_map, std::optional<int> {42});
    CHECK_FALSE(opt3);
  }
}

TEST_CASE("optional utilities")
{
  SUBCASE("merge_opt")
  {
    std::optional<int> a = 1;
    const std::optional<int> b = 2;
    merge_opt(a, b);
    CHECK(a == 2);

    const std::optional<int> c;
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

  SUBCASE("is_true_fnc with int-convertible types")
  {
    CHECK(is_true_fnc(std::optional<int> {1}));
    CHECK_FALSE(is_true_fnc(std::optional<int> {0}));
    CHECK_FALSE(is_true_fnc(std::optional<int> {}));
  }
}

TEST_CASE("C++23 optional monadic operations")
{
  const std::map<int, std::string> test_map {
      {1, "one"},
      {2, "two"},
      {3, "three"},
  };

  SUBCASE("get_optvalue_optkey uses and_then")
  {
    // and_then chains optional-returning functions
    auto opt = get_optvalue_optkey(test_map, std::optional<int> {2});
    REQUIRE(opt);
    CHECK(*opt == "two");

    // with nullopt key
    auto opt2 = get_optvalue_optkey(test_map, std::optional<int> {});
    CHECK_FALSE(opt2);

    // with key not in map
    auto opt3 = get_optvalue_optkey(test_map, std::optional<int> {99});
    CHECK_FALSE(opt3);
  }

  SUBCASE("optional value_or for boolean checks")
  {
    // is_true_fnc now uses value_or(false) - a C++23 pattern
    const std::optional<bool> opt_true {true};
    const std::optional<bool> opt_false {false};
    const std::optional<bool> opt_empty {};

    CHECK(is_true_fnc(opt_true));
    CHECK_FALSE(is_true_fnc(opt_false));
    CHECK_FALSE(is_true_fnc(opt_empty));
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

TEST_CASE("as_string tuple formatting")
{
  CHECK(as_string(std::tuple {1, 2}) == "(1, 2)");
  CHECK(as_string(std::tuple {std::string {"alpha"}, 3}) == "(alpha, 3)");
  CHECK(as_string(std::tuple {42}) == "(42)");
}

TEST_CASE("annual_discount_factor")
{
  constexpr double hours_per_year = 8760.0;  // Standard hours in a year

  SUBCASE("zero discount rate")
  {
    // With 0% discount rate, factor should always be 1.0 regardless of time
    CHECK(annual_discount_factor(0.0, 0.0) == doctest::Approx(1.0));
    CHECK(annual_discount_factor(0.0, 1000.0) == doctest::Approx(1.0));
    CHECK(annual_discount_factor(0.0, hours_per_year) == doctest::Approx(1.0));
  }

  SUBCASE("one year time period")
  {
    // For 1 year (8760 hours) with 5% discount rate
    // Formula: 1/(1 + r)^1 = 1/1.05 ≈ 0.95238095238
    CHECK(annual_discount_factor(0.05, hours_per_year)
          == doctest::Approx(0.95238095238));

    // For 1 year with 10% discount rate
    // 1/1.10 ≈ 0.90909090909
    CHECK(annual_discount_factor(0.10, hours_per_year)
          == doctest::Approx(0.90909090909));
  }

  SUBCASE("fractional year")
  {
    // Half year (4380 hours) with 10% discount rate
    // Formula: 1/(1 + r)^0.5 = 1/sqrt(1.10) ≈ 0.95346258925
    CHECK(annual_discount_factor(0.10, hours_per_year / 2)
          == doctest::Approx(0.95346258925));

    // Quarter year (2190 hours) with 5% discount rate
    // 1/(1.05)^0.25 ≈ 0.98788030338
    CHECK(annual_discount_factor(0.05, hours_per_year / 4)
          == doctest::Approx(0.98788030338));
  }

  SUBCASE("multiple years")
  {
    // 2 years (17520 hours) with 5% discount rate
    // Formula: 1/(1 + r)^2 = 1/1.1025 ≈ 0.90702947846
    CHECK(annual_discount_factor(0.05, 2 * hours_per_year)
          == doctest::Approx(0.90702947846));

    // 3 years (26280 hours) with 10% discount rate
    // 1/(1.10)^3 ≈ 0.7513148009
    CHECK(annual_discount_factor(0.10, 3 * hours_per_year)
          == doctest::Approx(0.7513148009));
  }

  SUBCASE("edge cases")
  {
    // Zero time should give 1.0 regardless of discount rate
    CHECK(annual_discount_factor(0.10, 0.0) == doctest::Approx(1.0));
    CHECK(annual_discount_factor(0.50, 0.0) == doctest::Approx(1.0));

    // Very high discount rate
    CHECK(annual_discount_factor(1.0, hours_per_year)
          == doctest::Approx(0.5));  // 1/(1+1) = 0.5
  }

  SUBCASE("negative discount rate")
  {
    // Negative discount rate means value increases over time
    // 1/(1 + (-0.05))^1 = 1/0.95 ≈ 1.05263157895
    CHECK(annual_discount_factor(-0.05, hours_per_year)
          == doctest::Approx(1.05263157895));
  }
}

TEST_CASE("enumerate empty range")
{
  std::vector<int> empty_vec;
  size_t count = 0;
  for ([[maybe_unused]] auto [idx, val] : enumerate(empty_vec)) {
    ++count;
  }
  CHECK(count == 0);
}

TEST_CASE("enumerate_active with all active")
{
  struct TestElement
  {
    bool active;
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }
  };

  const std::vector<TestElement> elements {{true}, {true}, {true}};
  size_t count = 0;
  for ([[maybe_unused]] auto [idx, elem] : enumerate_active(elements)) {
    ++count;
  }
  CHECK(count == 3);
}

TEST_CASE("enumerate_active with all inactive")
{
  struct TestElement
  {
    bool active;
    [[nodiscard]] constexpr bool is_active() const noexcept { return active; }
  };

  const std::vector<TestElement> elements {{false}, {false}, {false}};
  size_t count = 0;
  for ([[maybe_unused]] auto [idx, elem] : enumerate_active(elements)) {
    ++count;
  }
  CHECK(count == 0);
}

TEST_CASE("to_vector with empty range")
{
  std::vector<int> empty_vec;
  auto result = to_vector(empty_vec);
  CHECK(result.empty());
}

TEST_CASE("all_of with empty range")
{
  std::vector<int> empty_vec;
  CHECK(all_of(empty_vec, [](int) { return false; }));
}

TEST_CASE("merge_opt both empty")
{
  std::optional<int> a;
  const std::optional<int> b;
  merge_opt(a, b);
  CHECK_FALSE(a.has_value());
}

TEST_CASE("merge_opt dest has value, source empty")
{
  std::optional<int> a = 42;
  const std::optional<int> b;
  merge_opt(a, b);
  CHECK(a == 42);
}
