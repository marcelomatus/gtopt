TEST_CASE("Demand daw json test")
{
  using Uid = gtopt::Uid;

  const std::string_view json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300
    })";

  gtopt::Demand dem = daw::json::from_json<gtopt::Demand>(json_data);

  REQUIRE(dem.uid == 5);
  REQUIRE(dem.name == "GUACOLDA");
  REQUIRE(std::get<Uid>(dem.bus) == 10);
  if (dem.capacity) {
    REQUIRE(std::get<double>(dem.capacity.value()) == doctest::Approx(300));
  }
}

TEST_CASE("Demand daw json test 2")
{
  const std::string_view json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300
    })";

  gtopt::Demand dem = daw::json::from_json<gtopt::Demand>(json_data);

  REQUIRE(dem.uid == 5);
  REQUIRE(dem.name == "GUACOLDA");

  REQUIRE(std::get<gtopt::Uid>(dem.bus) == 10);
  if (dem.capacity) {
    REQUIRE(std::get<double>(dem.capacity.value()) == doctest::Approx(300));
  }
}

TEST_CASE("Demand daw json test 3")
{
  using Name = gtopt::Name;

  const std::string_view json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":"GOLDA",
    "capacity":300
    })";

  gtopt::Demand dem = daw::json::from_json<gtopt::Demand>(json_data);

  REQUIRE(dem.uid == 5);
  REQUIRE(dem.name == "GUACOLDA");
  REQUIRE(std::get<Name>(dem.bus) == "GOLDA");
  if (dem.capacity) {
    REQUIRE(std::get<double>(dem.capacity.value()) == doctest::Approx(300));
  }
}

TEST_CASE("Demand daw json test 4")
{
  std::string_view json_data = R"([{
    "uid":5,
    "name":"CRUCERO",
    "active": [0,0,1,1],
    "bus":"GOLDA"
    },{
    "uid":10,
    "name":"PTOMONTT",
    "bus":"GOLDA",
    }])";

  std::vector<gtopt::Demand> demand_a =
      daw::json::from_json_array<gtopt::Demand>(json_data);

  REQUIRE(demand_a[0].uid == 5);
  REQUIRE(demand_a[0].name == "CRUCERO");

  REQUIRE(demand_a[1].uid == 10);
  REQUIRE(demand_a[1].name == "PTOMONTT");

  using gtopt::False;
  using gtopt::True;

  const std::vector<gtopt::IntBool> empty;
  const std::vector<gtopt::IntBool> active = {False, False, True, True};

  REQUIRE(
      std::get<std::vector<gtopt::IntBool>>(demand_a[0].active.value_or(empty))
      == active);
}
