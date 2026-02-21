TEST_CASE("GeneratorProfile daw json test - basic fields")
{
  std::string_view json_data = R"({
    "uid":1,
    "name":"GPROFILE_A",
    "generator":10,
    "profile":0.95
  })";

  GeneratorProfile gp = daw::json::from_json<GeneratorProfile>(json_data);

  CHECK(gp.uid == 1);
  CHECK(gp.name == "GPROFILE_A");
  CHECK_FALSE(gp.active.has_value());
  CHECK(std::get<Uid>(gp.generator) == 10);
  CHECK(std::get<double>(gp.profile) == doctest::Approx(0.95));
}

TEST_CASE("GeneratorProfile daw json test - with cost and active")
{
  std::string_view json_data = R"({
    "uid":2,
    "name":"GPROFILE_B",
    "active":1,
    "generator":"GEN_REF",
    "profile":1.0,
    "scost":250.0
  })";

  GeneratorProfile gp = daw::json::from_json<GeneratorProfile>(json_data);

  CHECK(gp.uid == 2);
  CHECK(gp.name == "GPROFILE_B");
  REQUIRE(gp.active.has_value());
  CHECK(std::get<IntBool>(gp.active.value()) == True);  // NOLINT
  CHECK(std::get<Name>(gp.generator) == "GEN_REF");
  REQUIRE(gp.scost.has_value());
  CHECK(std::get<double>(gp.scost.value())  // NOLINT
        == doctest::Approx(250.0));
}

TEST_CASE("GeneratorProfile array json test")
{
  std::string_view json_data = R"([{
    "uid":1,
    "name":"GP_A",
    "generator":10,
    "profile":0.5
  },{
    "uid":2,
    "name":"GP_B",
    "generator":20,
    "profile":0.9
  }])";

  std::vector<GeneratorProfile> profiles =
      daw::json::from_json_array<GeneratorProfile>(json_data);

  REQUIRE(profiles.size() == 2);
  CHECK(profiles[0].uid == 1);
  CHECK(profiles[0].name == "GP_A");
  CHECK(profiles[1].uid == 2);
  CHECK(profiles[1].name == "GP_B");
}

TEST_CASE("GeneratorProfile round-trip serialization")
{
  GeneratorProfile gp;
  gp.uid = 7;
  gp.name = "RT_GPROFILE";
  gp.generator = Uid {99};
  gp.profile = 0.80;
  gp.scost = 300.0;

  auto json = daw::json::to_json(gp);
  GeneratorProfile roundtrip = daw::json::from_json<GeneratorProfile>(json);

  CHECK(roundtrip.uid == gp.uid);
  CHECK(roundtrip.name == gp.name);
  CHECK(std::get<Uid>(roundtrip.generator) == 99);
  CHECK(std::get<double>(roundtrip.profile) == doctest::Approx(0.80));
  REQUIRE(roundtrip.scost.has_value());
  CHECK(std::get<double>(roundtrip.scost.value()) ==  // NOLINT
        doctest::Approx(300.0));
}

