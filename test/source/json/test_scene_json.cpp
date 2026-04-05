/**
 * @file      test_scene_json.hpp
 * @brief     JSON serialization tests for Scene
 * @date      2026-03-18
 * @copyright BSD-3-Clause
 */

#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json_scene.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("Scene JSON basic parsing")
{
  const std::string_view json_data = R"({
    "uid": 1,
    "name": "winter",
    "first_scenario": 0,
    "count_scenario": 5
  })";

  const Scene scene = daw::json::from_json<Scene>(json_data);

  CHECK(scene.uid == 1);
  REQUIRE(scene.name.has_value());
  CHECK(scene.name.value_or("") == "winter");
  CHECK(scene.first_scenario == 0);
  CHECK(scene.count_scenario == 5);
}

TEST_CASE("Scene JSON minimal fields")
{
  const std::string_view json_data = R"({
    "uid": 2,
    "first_scenario": 3,
    "count_scenario": 10
  })";

  const Scene scene = daw::json::from_json<Scene>(json_data);

  CHECK(scene.uid == 2);
  CHECK_FALSE(scene.name.has_value());
  CHECK_FALSE(scene.active.has_value());
  CHECK(scene.first_scenario == 3);
  CHECK(scene.count_scenario == 10);
}

TEST_CASE("Scene JSON with active flag")
{
  const std::string_view json_data = R"({
    "uid": 3,
    "name": "summer",
    "active": 0,
    "first_scenario": 0,
    "count_scenario": 1
  })";

  const Scene scene = daw::json::from_json<Scene>(json_data);

  REQUIRE(scene.active.has_value());
  CHECK(scene.active.value_or(true) == false);
  CHECK_FALSE(scene.is_active());
}

TEST_CASE("Scene JSON array parsing")
{
  const std::string_view json_data = R"([
    {"uid": 1, "name": "s1", "first_scenario": 0, "count_scenario": 3},
    {"uid": 2, "name": "s2", "first_scenario": 3, "count_scenario": 2}
  ])";

  const auto scenes = daw::json::from_json_array<Scene>(json_data);

  REQUIRE(scenes.size() == 2);
  CHECK(scenes[0].uid == 1);
  CHECK(scenes[0].first_scenario == 0);
  CHECK(scenes[0].count_scenario == 3);
  CHECK(scenes[1].uid == 2);
  CHECK(scenes[1].first_scenario == 3);
}

TEST_CASE("Scene JSON round-trip serialization")
{
  Scene original;
  original.uid = 5;
  original.name = "roundtrip_scene";
  original.active = true;
  original.first_scenario = 2;
  original.count_scenario = 7;

  const auto json = daw::json::to_json(original);
  CHECK(!json.empty());

  const Scene roundtrip = daw::json::from_json<Scene>(json);
  CHECK(roundtrip.uid == 5);
  CHECK(roundtrip.name.value_or("") == "roundtrip_scene");
  CHECK(roundtrip.first_scenario == 2);
  CHECK(roundtrip.count_scenario == 7);
}
