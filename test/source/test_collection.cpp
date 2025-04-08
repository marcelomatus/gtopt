#include <ranges>
#include <string>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/collection.hpp>

using namespace gtopt;

struct test_object
{
  constexpr static std::string_view ClassName = "test_object";
  Uid uid {};
  Name name {};
  int value {};

  [[nodiscard]] auto id() const -> std::pair<Uid, NameView>
  {
    return {uid, name};
  }
};

TEST_CASE("Collection test 1")
{
  std::vector<test_object> vec1 = {{.uid = 1, .name = "n1", .value = 1},
                                   {.uid = 2, .name = "n2", .value = 2},
                                   {.uid = 3, .name = "n3", .value = 3}};

  REQUIRE(vec1.size() == 3);
  REQUIRE(vec1[0].uid == 1);
  REQUIRE(vec1[1].uid == 2);
  REQUIRE(vec1[2].uid == 3);

  const gtopt::Collection<test_object> coll {vec1};

  REQUIRE(coll.size() == 3);

  {
    auto&& el1 = coll.element(Uid {1});

    REQUIRE(el1.uid == 1);
    REQUIRE(el1.name == "n1");
    REQUIRE(el1.value == 1);
  }
  {
    const std::string name = "n3";
    auto&& el1 = coll.element(name);

    REQUIRE(el1.uid == 3);
    REQUIRE(el1.name == "n3");
    REQUIRE(el1.value == 3);
  }
  {
    const gtopt::SingleId id1 = {Uid {1}};
    auto&& el1 = coll.element(id1);
    REQUIRE(el1.uid == 1);
    REQUIRE(el1.name == "n1");
    REQUIRE(el1.value == 1);
  }

  {
    const gtopt::SingleId id1 = {"n2"};
    auto&& el1 = coll.element(id1);
    REQUIRE(el1.uid == 2);
    REQUIRE(el1.name == "n2");
    REQUIRE(el1.value == 2);
  }

  {
    gtopt::Collection<test_object> coll1(coll);
    gtopt::Collection<test_object> coll2 = coll1;

    coll1 = coll2;
    coll2 = coll1;

    auto const& vec2 = coll2.elements();
    REQUIRE(vec2.size() == 3);
    REQUIRE(vec2[0].uid == 1);
    REQUIRE(vec2[1].uid == 2);
    REQUIRE(vec2[2].uid == 3);
  }
}

// Helper class to test with
class MyCollection
{
public:
  MyCollection(std::initializer_list<int> items)
      : data(items)
  {
  }

  auto elements() const { return data; }  // NOLINT
  auto elements() { return data; }

private:
  std::vector<int> data;
};

TEST_CASE("visit_elements basic functionality")
{
  MyCollection c1 {1, 2, 3, 4, 5};
  MyCollection c2 {6, 7, 8, 9, 10};
  auto collections = std::make_tuple(c1, c2);

  SUBCASE("Count all elements")
  {
    auto count = visit_elements(collections, [](int) { return true; });
    CHECK(count == 10);
  }

  SUBCASE("Count even elements")
  {
    auto count = visit_elements(collections, [](int n) { return n % 2 == 0; });
    CHECK(count == 5);  // 2, 4, 6, 8, 10
  }

  SUBCASE("Count elements greater than 5")
  {
    auto count = visit_elements(collections, [](int n) { return n > 5; });
    CHECK(count == 5);  // 6, 7, 8, 9, 10
  }

  SUBCASE("Count elements that match no condition")
  {
    auto count = visit_elements(collections, [](int n) { return n > 100; });
    CHECK(count == 0);
  }
}

TEST_CASE("visit_elements with different collection types")
{
  class StringMyCollection
  {
  public:
    StringMyCollection(std::initializer_list<std::string> items)
        : data(items)
    {
    }
    auto elements() const { return data; }  // NOLINT

  private:
    std::vector<std::string> data;
  };

  MyCollection c1 {1, 2, 3};
  StringMyCollection c2 {"a", "bb", "ccc"};
  auto collections = std::make_tuple(c1, c2);

  SUBCASE("Count elements with specific properties")
  {
    int intCount = 0;
    int strCount = 0;

    auto count =
        visit_elements(collections,
                       [&](auto&& elem)
                       {
                         using Type = std::decay_t<decltype(elem)>;
                         if constexpr (std::is_same_v<Type, int>) {
                           intCount++;
                           return elem > 1;
                         } else if constexpr (std::is_same_v<Type, std::string>)
                         {
                           strCount++;
                           return elem.length() > 1;
                         }
                         return false;
                       });

    CHECK(intCount == 3);  // All integers are visited
    CHECK(strCount == 3);  // All strings are visited
    CHECK(
        count
        == 4);  // 2 integers (2,3) and 2 strings ("bb","ccc") meet the criteria
  }
}

TEST_CASE("visit_elements constexpr usage")
{
  // This test verifies that visit_elements works in constexpr context
  struct ConstMyCollection
  {
    constexpr auto elements() const  // NOLINT
    {
      return std::array {1, 2, 3, 4, 5};
    }
  };

  constexpr ConstMyCollection cc;
  constexpr auto collections = std::make_tuple(cc);

  constexpr auto count =
      visit_elements(collections, [](int n) { return n % 2 == 0; });
  static_assert(count == 2, "Should count 2 even numbers");
  CHECK(count == 2);  // Also check at runtime
}
