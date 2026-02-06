#include <string>

#include <doctest/doctest.h>
#include <gtopt/basic_types.hpp>
#include <gtopt/utils.hpp>

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
  return ranges::views::zip(
      ranges::views::iota(0)
          | ranges::views::transform([](size_t i)
                                     { return static_cast<IndexType>(i); }),
      range);
}

TEST_CASE("gtopt types 2")  // NOLINT
{
  std::vector<int> vec1 = {3, 4, 5, 6, 7};  // NOLINT
  std::vector<int> vec2 = {1, 2, 3, 4, 5};  // NOLINT

  auto rng = ranges::views::zip(vec1, vec2);

  for (size_t i = 0; auto&& [v1, v2] : rng) {
    REQUIRE(v1 == vec1[i]);
    REQUIRE(v2 == vec2[i]);
    ++i;
  }

  auto rng2 = ranges::views::zip(
      ranges::views::iota(0)
          | ranges::views::transform([](size_t i) { return i; }),
      vec1);
  for (auto&& [i, v1] : rng2) {
    REQUIRE(v1 == vec1[i]);
  }

  for (auto&& [i, v1] : my_enumerate(vec1)) {
    REQUIRE(v1 == vec1[i]);
  }
}
