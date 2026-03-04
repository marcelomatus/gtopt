/**
 * @file      test_single_id.cpp
 * @brief     Unit tests for SingleId utilities
 * @date      Sat Feb  8 07:37:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * Tests for SingleId, ObjectSingleId, and related utilities
 */

#include <string>

#include <doctest/doctest.h>
#include <gtopt/single_id.hpp>

using namespace gtopt;

namespace
{
// Test object types
struct TestObject1
{
  Uid object_uid;
  Name object_name;

  [[nodiscard]] constexpr auto uid() const -> Uid { return object_uid; }
  [[nodiscard]] constexpr auto name() const -> const Name&
  {
    return object_name;
  }
};

struct TestObject2
{
  Uid object_uid;
  Name object_name;

  [[nodiscard]] constexpr auto uid() const -> Uid { return object_uid; }
  [[nodiscard]] constexpr auto name() const -> const Name&
  {
    return object_name;
  }
};
}  // namespace

TEST_CASE("get_uid and get_name - With objects")
{
  const TestObject1 obj {.object_uid = Uid {42}, .object_name = "test_obj"};

  CHECK(get_uid(obj) == Uid {42});
  CHECK(get_name(obj) == "test_obj");
}

TEST_CASE("get_uid and get_name - With Id")
{
  const Id id {Uid {100}, "test_id"};

  CHECK(get_uid(id) == Uid {100});
  CHECK(get_name(id) == "test_id");
}

TEST_CASE("ObjectId - Construction and access")
{
  const Id base_id {Uid {5}, "base"};
  const ObjectId<TestObject1> obj_id {base_id};

  CHECK(obj_id.first == Uid {5});
  CHECK(obj_id.second == "base");
}

TEST_CASE("SingleId - Basic variant functionality")
{
  SUBCASE("Construct with Uid")
  {
    SingleId sid = Uid {42};
    CHECK(std::holds_alternative<Uid>(sid));
    CHECK(std::get<Uid>(sid) == 42);
  }

  SUBCASE("Construct with Name")
  {
    SingleId sid = Name {"test_name"};
    CHECK(std::holds_alternative<Name>(sid));
    CHECK(std::get<Name>(sid) == "test_name");
  }
}

TEST_CASE("OptSingleId - Optional variant")
{
  SUBCASE("Empty optional")
  {
    const OptSingleId opt;
    CHECK_FALSE(opt.has_value());
  }

  SUBCASE("Optional with Uid")
  {
    OptSingleId opt = SingleId {Uid {10}};
    CHECK(opt.has_value());
    CHECK(std::holds_alternative<Uid>(*opt));
    CHECK(std::get<Uid>(*opt) == 10);
  }

  SUBCASE("Optional with Name")
  {
    OptSingleId opt = SingleId {Name {"optional_name"}};
    CHECK(opt.has_value());
    CHECK(std::holds_alternative<Name>(*opt));
    CHECK(std::get<Name>(*opt) == "optional_name");
  }
}

TEST_CASE("ObjectSingleId - Construction")
{
  SUBCASE("Default construction")
  {
    const ObjectSingleId<TestObject1> obj_sid;
    // Should be default-constructed variant (holds Uid by default)
    CHECK(std::holds_alternative<Uid>(obj_sid));
  }

  SUBCASE("Construct from Uid")
  {
    const ObjectSingleId<TestObject1> obj_sid {Uid {99}};
    CHECK(std::holds_alternative<Uid>(obj_sid));
    CHECK(obj_sid.uid() == 99);
  }

  SUBCASE("Construct from Name")
  {
    const ObjectSingleId<TestObject1> obj_sid {Name {"obj_name"}};
    CHECK(std::holds_alternative<Name>(obj_sid));
    CHECK(obj_sid.name() == "obj_name");
  }

  SUBCASE("Construct from SingleId (copy)")
  {
    const SingleId sid = Uid {123};
    const ObjectSingleId<TestObject1> obj_sid {sid};
    CHECK(std::holds_alternative<Uid>(obj_sid));
    CHECK(obj_sid.uid() == 123);
  }

  SUBCASE("Construct from SingleId (move)")
  {
    SingleId sid = Name {"moved_name"};
    const ObjectSingleId<TestObject1> obj_sid {std::move(sid)};
    CHECK(std::holds_alternative<Name>(obj_sid));
    CHECK(obj_sid.name() == "moved_name");
  }
}

TEST_CASE("ObjectSingleId - Copy and move semantics")
{
  SUBCASE("Copy construction")
  {
    const ObjectSingleId<TestObject1> obj_sid1 {Uid {50}};
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const ObjectSingleId<TestObject1> obj_sid2 {obj_sid1};
    CHECK(obj_sid2.uid() == 50);
  }

  SUBCASE("Copy assignment")
  {
    const ObjectSingleId<TestObject1> obj_sid1 {Uid {60}};
    ObjectSingleId<TestObject1> obj_sid2 {Uid {70}};
    obj_sid2 = obj_sid1;
    CHECK(obj_sid2.uid() == 60);
  }

  SUBCASE("Move construction")
  {
    ObjectSingleId<TestObject1> obj_sid1 {Name {"moving"}};
    const ObjectSingleId<TestObject1> obj_sid2 {std::move(obj_sid1)};
    CHECK(obj_sid2.name() == "moving");
  }

  SUBCASE("Move assignment")
  {
    ObjectSingleId<TestObject1> obj_sid1 {Name {"moved"}};
    ObjectSingleId<TestObject1> obj_sid2 {Uid {0}};
    obj_sid2 = std::move(obj_sid1);
    CHECK(obj_sid2.name() == "moved");
  }
}

TEST_CASE("ObjectSingleId - Accessors")
{
  SUBCASE("uid() accessor")
  {
    const ObjectSingleId<TestObject1> obj_sid {Uid {200}};
    CHECK(obj_sid.uid() == 200);
  }

  SUBCASE("name() accessor")
  {
    const ObjectSingleId<TestObject1> obj_sid {Name {"accessor_test"}};
    CHECK(obj_sid.name() == "accessor_test");
  }
}

TEST_CASE("ObjectSingleId - Type safety")
{
  // Different object types should have different ObjectSingleId types
  using ObjSid1 = ObjectSingleId<TestObject1>;
  using ObjSid2 = ObjectSingleId<TestObject2>;

  static_assert(!std::is_convertible_v<ObjSid1, ObjSid2>,
                "ObjectSingleId should enforce type safety");
  static_assert(!std::is_convertible_v<ObjSid2, ObjSid1>,
                "ObjectSingleId should enforce type safety");

  // Should be able to identify the object type
  static_assert(std::is_same_v<ObjSid1::object_type, TestObject1>);
  static_assert(std::is_same_v<ObjSid2::object_type, TestObject2>);
}

TEST_CASE("ObjectSingleId - Variant visitation")
{
  ObjectSingleId<TestObject1> obj_sid_uid {Uid {77}};
  ObjectSingleId<TestObject1> obj_sid_name {Name {"visit_test"}};

  SUBCASE("Visit with Uid")
  {
    auto result = std::visit(
        [](auto&& value) -> std::string
        {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, Uid>) {
            return std::format("Uid: {}", value);
          } else {
            return std::format("Name: {}", value);
          }
        },
        obj_sid_uid);

    CHECK(result == "Uid: 77");
  }

  SUBCASE("Visit with Name")
  {
    auto result = std::visit(
        [](auto&& value) -> std::string
        {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, Uid>) {
            return std::format("Uid: {}", value);
          } else {
            return std::format("Name: {}", value);
          }
        },
        obj_sid_name);

    CHECK(result == "Name: visit_test");
  }
}

TEST_CASE("ObjectSingleId - Use in containers")
{
  std::vector<ObjectSingleId<TestObject1>> vec;
  vec.emplace_back(Uid {1});
  vec.emplace_back(Name {"second"});
  vec.emplace_back(Uid {3});

  CHECK(vec.size() == 3);
  CHECK(std::holds_alternative<Uid>(vec[0]));
  CHECK(std::holds_alternative<Name>(vec[1]));
  CHECK(std::holds_alternative<Uid>(vec[2]));
}
