// SPDX-License-Identifier: BSD-3-Clause
#include <string>
#include <string_view>

#include <daw/json/daw_json_link.h>
#include <doctest/doctest.h>
#include <gtopt/json_schema.hpp>

using namespace gtopt;

// Wrap file-local helpers in a uniquely-named outer namespace so the
// unity build cannot collide anonymous-namespace symbols across test
// translation units (see the unity-anon-namespace project feedback).
namespace test_json_schema_detail
{
// Anonymous namespace for internal linkage; the uniquely-named parent keeps
// these helpers from colliding with other test files under unity builds.
namespace
{

/// Parse @p text as a generic JSON value; returns true iff it is
/// well-formed JSON.  Uses daw_json_link's DOM entry point so we exercise
/// the same library that produced the schema string.
[[nodiscard]] bool is_valid_json(std::string_view text)
{
  try {
    auto val = daw::json::json_value(text);
    // Touch a member so the lazy parser actually validates the document.
    (void)val.find_class_member("$schema");
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

}  // namespace test_json_schema_detail

TEST_CASE("dump_json_schema default (options) schema")  // NOLINT
{
  using test_json_schema_detail::is_valid_json;

  SUBCASE("default (empty name) yields a valid options schema")
  {
    const std::string schema = dump_json_schema();

    CHECK(!schema.empty());
    CHECK(schema.front() == '{');
    CHECK(is_valid_json(schema));

    // Draft 2020-12 schema preamble and object shape.
    CHECK(schema.find(R"("$schema")") != std::string::npos);
    CHECK(schema.find(R"("type")") != std::string::npos);
    CHECK(schema.find("gtopt.PlanningOptions") != std::string::npos);

    // PlanningOptions' json_data_contract maps these properties.
    CHECK(schema.find(R"("input_directory")") != std::string::npos);
    CHECK(schema.find(R"("output_directory")") != std::string::npos);
    CHECK(schema.find(R"("method")") != std::string::npos);
    CHECK(schema.find(R"("sddp_options")") != std::string::npos);
  }

  SUBCASE("explicit 'options' matches the default")
  {
    CHECK(dump_json_schema("options") == dump_json_schema());
  }

  SUBCASE("name match is case-insensitive")
  {
    CHECK(dump_json_schema("OPTIONS") == dump_json_schema());
  }
}

TEST_CASE("dump_json_schema known names render valid JSON")  // NOLINT
{
  using test_json_schema_detail::is_valid_json;

  SUBCASE("known_schema_names is non-empty and 'options' is the default")
  {
    const auto& names = known_schema_names();
    REQUIRE(!names.empty());
    CHECK(names.front() == std::string_view {"options"});
  }

  SUBCASE("every advertised name renders valid, non-empty JSON")
  {
    for (const auto name : known_schema_names()) {
      const std::string schema = dump_json_schema(name);
      CHECK(!schema.empty());
      CHECK(is_valid_json(schema));
    }
  }
}

TEST_CASE("dump_json_schema unknown type returns empty")  // NOLINT
{
  SUBCASE("unknown name yields an empty string")
  {
    // dump_json_schema prints an error to stderr and returns empty.
    CHECK(dump_json_schema("definitely-not-a-type").empty());
  }
}
