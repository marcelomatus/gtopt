TEST_CASE("JSON Planning - Serialize empty")
{

  // Create empty planning
  const Planning opt {};

  // Serialize to JSON
  const auto json_data = daw::json::to_json(opt);

  // Verify JSON structure
  CHECK(json_data.find("\"options\":") != std::string::npos);
  CHECK(json_data.find("\"simulation\":") != std::string::npos);
  CHECK(json_data.find("\"system\":") != std::string::npos);
}

TEST_CASE("JSON Planning - Round trip serialization")
{

  // Create planning with components
  const Options options {};
  const Simulation simulation {};

  // Create minimal system
  const Array<Bus> bus_array = {{.uid = Uid {1}, .name = "b1"}};
  const Array<Generator> gen_array = {
      {.uid = Uid {1}, .name = "g1", .bus = Uid {1}},
  };
  const System system {
      .name = "TestSystem",
      .bus_array = bus_array,
      .generator_array = gen_array,
  };

  Planning original {
      .options = options,
      .simulation = simulation,
      .system = system,
  };

  // Serialize to JSON
  const auto json_data = daw::json::to_json(original);

  // Deserialize back to an object
  const auto deserialized = daw::json::from_json<Planning>(json_data);

  // Verify the deserialized object matches the original
  CHECK(deserialized.system.name == original.system.name);

  REQUIRE(deserialized.system.bus_array.size()
          == original.system.bus_array.size());
  CHECK(deserialized.system.bus_array[0].name
        == original.system.bus_array[0].name);

  REQUIRE(deserialized.system.generator_array.size()
          == original.system.generator_array.size());
  CHECK(deserialized.system.generator_array[0].name
        == original.system.generator_array[0].name);
}

TEST_CASE("JSON Planning - Partial filled objects")
{

  // Create planning with only some components filled
  const Planning opt1 {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {},  // NOLINT
  };

  // Serialize and deserialize
  const auto json_data1 = daw::json::to_json(opt1);
  const auto deserialized1 = daw::json::from_json<Planning>(json_data1);

  // Check that partial filling works correctly
  CHECK(deserialized1.system.name.empty());

  // Another test with different components filled
  const Planning opt2 {
      .options = {},  // NOLINT
      .simulation = {},  // NOLINT
      .system = {.name = "TestSystem"},
  };

  // Serialize and deserialize
  const auto json_data2 = daw::json::to_json(opt2);
  const auto deserialized2 = daw::json::from_json<Planning>(json_data2);

  // Check that partial filling works correctly
  CHECK(deserialized2.system.name == "TestSystem");
}

