TEST_CASE("Generator daw json test 1")
{
  using Uid = gtopt::Uid;

  std::string_view test_001_t_json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":10,
    "capacity":300,
    "pmin":0,
    "pmax":275.5
    })";

  gtopt::Generator gen =
      daw::json::from_json<gtopt::Generator>(test_001_t_json_data);

  REQUIRE(gen.uid == 5);
  REQUIRE(gen.name == "GUACOLDA");
  REQUIRE(std::get<Uid>(gen.bus) == 10);
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value_or(-1.0))
          == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));
}

TEST_CASE("Generator daw json test 2")
{
  using Name = gtopt::Name;

  std::string_view test_001_t_json_data = R"({
    "uid":5,
    "name":"GUACOLDA",
    "bus":"GUACOLDA",
    "capacity":300,
    "pmin":0,
    "pmax":275.5
    })";

  gtopt::Generator gen =
      daw::json::from_json<gtopt::Generator>(test_001_t_json_data);

  REQUIRE(gen.uid == 5);
  REQUIRE(gen.name == "GUACOLDA");
  REQUIRE(std::get<Name>(gen.bus) == "GUACOLDA");
  REQUIRE(std::get<double>(gen.pmin.value_or(-1.0)) == doctest::Approx(0));
  REQUIRE(std::get<double>(gen.capacity.value_or(-1.0))
          == doctest::Approx(300));
  REQUIRE(std::get<double>(gen.pmax.value_or(-1.0)) == doctest::Approx(275.5));
}

