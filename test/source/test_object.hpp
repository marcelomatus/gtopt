// SPDX-License-Identifier: BSD-3-Clause
#include <doctest/doctest.h>
#include <gtopt/object.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("ObjectAttrs construction and default values")
{
  const ObjectAttrs attrs;

  CHECK(attrs.uid == Uid {unknown_uid});
  CHECK(attrs.name == Name {});
  CHECK_FALSE(attrs.active.has_value());
}

TEST_CASE("ObjectAttrs attribute assignment")
{
  ObjectAttrs attrs;

  attrs.uid = 42;
  attrs.name = "test_object";
  attrs.active = true;

  CHECK(attrs.uid == 42);
  CHECK(attrs.name == "test_object");
  REQUIRE(attrs.active.has_value());
  CHECK(std::get<IntBool>(attrs.active.value()) == 1);
}

TEST_CASE("ObjectAttrs designated initializer construction")
{
  const ObjectAttrs attrs {
      .uid = Uid {99},
      .name = "named_object",
      .active = Active {true},
  };

  CHECK(attrs.uid == Uid {99});
  CHECK(attrs.name == "named_object");
  REQUIRE(attrs.active.has_value());
}

TEST_CASE("ObjectAttrs with inactive status")
{
  ObjectAttrs attrs;
  attrs.active = false;

  REQUIRE(attrs.active.has_value());
  CHECK(std::get<IntBool>(attrs.active.value()) == 0);
}

TEST_CASE("id() free function creates Id from object")  // NOLINT
{
  struct TestObj
  {
    Uid uid {10};
    Name name {"my_obj"};
  };

  const TestObj obj;
  const auto obj_id = id(obj);

  CHECK(obj_id.uid == Uid {10});
  CHECK(obj_id.name == "my_obj");
}

TEST_CASE("id() with default-constructed object")  // NOLINT
{
  struct TestObj
  {
    Uid uid {unknown_uid};
    Name name {};
  };

  const TestObj obj;
  const auto obj_id = id(obj);

  CHECK(obj_id.uid == Uid {unknown_uid});
  CHECK(obj_id.name == Name {});
}
