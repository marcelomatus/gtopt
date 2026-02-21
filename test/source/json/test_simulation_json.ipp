#ifdef NONE

TEST_CASE("Simulation daw json test 1")
{
  const std::string_view json_data = R"({
    "name":"SEN",
    "block_array":[{"uid": 1, "duration": 2}],
    "stage_array":[],
    "scenario_array":[]})";

  gtopt::Simulation sys = daw::json::from_json<gtopt::Simulation>(json_data);

  using gtopt::False;
  using gtopt::True;

  REQUIRE(sys.block_array.size() == 1);
  REQUIRE(sys.block_array[0].uid == 1);
  REQUIRE(sys.block_array[0].duration == 2);

  REQUIRE(sys.stage_array.size() == 0);
  REQUIRE(sys.scenario_array.size() == 0);
}

TEST_CASE("Simulation daw json test 3")
{
  const size_t size = 1000;

  std::vector<gtopt::Bus> bus_array(size);
  std::vector<gtopt::Generator> generator_array(size);
  const std::vector<gtopt::Demand> demand_array;
  const std::vector<gtopt::Line> line_array;

  {
    gtopt::Uid uid = 0;

    for (size_t i = 0; i < size; ++i) {
      const gtopt::SingleId bus {uid};
      bus_array[i] = {.uid = uid, .name = "bus"};
      generator_array[i] = {.uid = uid,
                            .name = "gen",
                            .bus = bus,
                            .pmin = 0.0,
                            .pmax = 300.0,
                            .gcost = 50.0,
                            .capacity = 300.0};
      ++uid;
    }
  }

  const gtopt::Simulation simulation {
      .block_array = {}, .stage_array = {}, .scenario_array = {}};

  auto json_data = daw::json::to_json(simulation);

  gtopt::Simulation sys;

  // BENCHMARK("read simulation")
  {
    sys = daw::json::from_json<gtopt::Simulation>(json_data);
    // return sys.name;
  };
}

#endif

