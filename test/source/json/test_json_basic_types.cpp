#include <tuple>
#include <variant>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_basic_types.hpp>
#include <gtopt/json/json_field_sched.hpp>

namespace
{
struct Foo
{
  double f1 {};
};

using variant_t = std::variant<double, std::vector<double>, gtopt::FileSched>;
struct MyClass
{
  double f1;
  std::vector<double> f2;
  gtopt::FileSched f3;
  variant_t f4;
  std::optional<variant_t> f5;
};

}  // namespace

namespace daw::json
{

template<>
struct json_data_contract<Foo>
{
  using type = json_member_list<json_number<"f1", double>>;

  static auto to_json_data(Foo const& mc)
  {
    return std::forward_as_tuple(mc.f1);
  }
};

template<>
struct json_data_contract<MyClass>
{
  using type = json_member_list<
      json_number<"f1", double>,
      json_array<"f2", double>,
      json_string<"f3", gtopt::FileSched>,
      json_variant<"f4", variant_t, jvtl_RealFieldSched>,
      json_variant_null<"f5", std::optional<variant_t>, jvtl_RealFieldSched>>;

  static auto to_json_data(MyClass const& mc)
  {
    return std::forward_as_tuple(mc.f1, mc.f2, mc.f3, mc.f4, mc.f5);
  }
};

}  // namespace daw::json

TEST_CASE("daw json gtopt basic types 1")
{
  const double f1 = 3.5;
  const std::vector<double> f2 = {1, 2, 3, 4};
  const gtopt::FileSched f3 = "f1";
  const variant_t f4 = f1;
  const std::optional<variant_t> f5;

  const MyClass mc0 = {f1, f2, f3, f4, f5};  // NOLINT

  const auto json_data_4 = daw::json::to_json(mc0);

  const std::string_view json_data = R"({
     "f1": 3.5,
     "f2": [1,2,3,4],
     "f3": "f1",
     "f4": 3.5
    })";

  //

  {
    const MyClass mc = daw::json::from_json<MyClass>(json_data_4);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
  {
    auto mc = daw::json::from_json<MyClass>(json_data);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
}

TEST_CASE("daw json gtopt basic types 2")
{
  const double f1 = 3.5;
  const std::vector<double> f2 = {1, 2, 3, 4};
  const gtopt::FileSched f3 = "f1";
  const variant_t f4 = f1;
  const std::optional<variant_t> f5 = f3;

  const MyClass mc0 = {f1, f2, f3, f4, f5};  // NOLINT

  const auto json_data_4 = daw::json::to_json(mc0);

  const std::string_view json_data = R"({
     "f1": 3.5,
     "f2": [1,2,3,4],
     "f3": "f1",
     "f4": 3.5,
     "f5": "f1"
    })";

  //

  {
    const auto mc = daw::json::from_json<MyClass>(json_data_4);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
  {
    const auto mc = daw::json::from_json<MyClass>(json_data);
    REQUIRE(mc.f1 == doctest::Approx(f1));
    REQUIRE(mc.f2 == f2);
    REQUIRE(mc.f3 == f3);
    REQUIRE(mc.f4 == f4);
    REQUIRE(mc.f5 == f5);
  }
}
