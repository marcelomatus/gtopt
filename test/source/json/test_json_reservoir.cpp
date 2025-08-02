#include <string_view>
#include <vector>

#include <doctest/doctest.h>
#include <gtopt/json/json.hpp>
#include <gtopt/reservoir.hpp>

using namespace gtopt;

TEST_CASE("Reservoir basic fields serialization")
{
    Reservoir res {
        .uid = Uid{123},
        .name = "TestReservoir",
        .active = true,
        .junction = SingleId{Uid{456}}
    };

    nlohmann::json j = res;
    
    CHECK(j["uid"] == 123);
    CHECK(j["name"] == "TestReservoir");
    CHECK(j["active"] == true);
    CHECK(j["junction"] == 456);
}

TEST_CASE("Reservoir optional fields serialization")
{
    Reservoir res {
        .capacity = OptTRealFieldSched{1.0},
        .annual_loss = OptTRealFieldSched{0.05},
        .vmin = OptTRealFieldSched{100.0},
        .vmax = OptTRealFieldSched{1000.0},
        .vcost = OptTRealFieldSched{5.0},
        .vini = OptReal{500.0},
        .vfin = OptReal{600.0}
    };

    nlohmann::json j = res;
    
    CHECK(j["capacity"] == 1.0);
    CHECK(j["annual_loss"] == 0.05);
    CHECK(j["vmin"] == 100.0);
    CHECK(j["vmax"] == 1000.0);
    CHECK(j["vcost"] == 5.0);
    CHECK(j["vini"] == 500.0);
    CHECK(j["vfin"] == 600.0);
}

TEST_CASE("Reservoir basic fields deserialization")
{
    std::string_view json_str = R"({
        "uid": 123,
        "name": "TestReservoir",
        "active": true,
        "junction": 456
    })";
    
    auto j = nlohmann::json::parse(json_str);
    Reservoir r = j;
    
    CHECK(r.uid == Uid{123});
    CHECK(r.name == "TestReservoir");
    CHECK(r.active == true);
    CHECK(r.junction == SingleId{Uid{456}});
}

TEST_CASE("Reservoir optional fields deserialization")
{
    std::string_view json_str = R"({
        "uid": 123,
        "capacity": 1.0,
        "annual_loss": 0.05,
        "vmin": 100.0,
        "vmax": 1000.0,
        "vcost": 5.0,
        "vini": 500.0,
        "vfin": 600.0
    })";
    
    auto j = nlohmann::json::parse(json_str);
    Reservoir r = j;
    
    CHECK(r.capacity.value() == 1.0);
    CHECK(r.annual_loss.value() == 0.05);
    CHECK(r.vmin.value() == 100.0);
    CHECK(r.vmax.value() == 1000.0);
    CHECK(r.vcost.value() == 5.0);
    CHECK(r.vini.value() == 500.0);
    CHECK(r.vfin.value() == 600.0);
}

TEST_CASE("Reservoir roundtrip serialization")
{
    Reservoir original {
        .uid = Uid{123},
        .name = "TestReservoir",
        .active = true,
        .junction = SingleId{Uid{456}},
        .capacity = OptTRealFieldSched{1.0},
        .annual_loss = OptTRealFieldSched{0.05},
        .vmin = OptTRealFieldSched{100.0},
        .vmax = OptTRealFieldSched{1000.0},
        .vcost = OptTRealFieldSched{5.0},
        .vini = OptReal{500.0},
        .vfin = OptReal{600.0}
    };

    nlohmann::json j = original;
    Reservoir roundtrip = j;
    
    CHECK(roundtrip.uid == original.uid);
    CHECK(roundtrip.name == original.name);
    CHECK(roundtrip.active == original.active);
    CHECK(roundtrip.junction == original.junction);
    CHECK(roundtrip.capacity.value() == original.capacity.value());
    CHECK(roundtrip.annual_loss.value() == original.annual_loss.value());
    CHECK(roundtrip.vmin.value() == original.vmin.value());
    CHECK(roundtrip.vmax.value() == original.vmax.value());
    CHECK(roundtrip.vcost.value() == original.vcost.value());
    CHECK(roundtrip.vini.value() == original.vini.value());
    CHECK(roundtrip.vfin.value() == original.vfin.value());
}
