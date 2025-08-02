/**
 * @file      test_json_reservoir.cpp
 * @brief     Unit tests for Reservoir JSON serialization/deserialization
 * @date      Wed Jul 30 23:22:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module contains unit tests for the JSON serialization and deserialization
 * of Reservoir objects.
 */

#include <gtest/gtest.h>
#include <gtopt/reservoir.hpp>
#include <gtopt/json/json.hpp>
#include <nlohmann/json.hpp>

namespace gtopt
{

class TestJsonReservoir : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a test reservoir with all fields populated
        test_reservoir = {
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
    }

    Reservoir test_reservoir;
};

TEST_F(TestJsonReservoir, SerializeBasicFields)
{
    nlohmann::json j = test_reservoir;
    
    EXPECT_EQ(j["uid"], 123);
    EXPECT_EQ(j["name"], "TestReservoir");
    EXPECT_EQ(j["active"], true);
    EXPECT_EQ(j["junction"], 456);
}

TEST_F(TestJsonReservoir, SerializeOptionalFields)
{
    nlohmann::json j = test_reservoir;
    
    EXPECT_DOUBLE_EQ(j["capacity"], 1.0);
    EXPECT_DOUBLE_EQ(j["annual_loss"], 0.05);
    EXPECT_DOUBLE_EQ(j["vmin"], 100.0);
    EXPECT_DOUBLE_EQ(j["vmax"], 1000.0);
    EXPECT_DOUBLE_EQ(j["vcost"], 5.0);
    EXPECT_DOUBLE_EQ(j["vini"], 500.0);
    EXPECT_DOUBLE_EQ(j["vfin"], 600.0);
}

TEST_F(TestJsonReservoir, DeserializeBasicFields)
{
    const char* json_str = R"({
        "uid": 123,
        "name": "TestReservoir",
        "active": true,
        "junction": 456
    })";
    
    auto j = nlohmann::json::parse(json_str);
    Reservoir r = j;
    
    EXPECT_EQ(r.uid, Uid{123});
    EXPECT_EQ(r.name, "TestReservoir");
    EXPECT_EQ(r.active, true);
    EXPECT_EQ(r.junction, SingleId{Uid{456}});
}

TEST_F(TestJsonReservoir, DeserializeOptionalFields)
{
    const char* json_str = R"({
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
    
    EXPECT_DOUBLE_EQ(r.capacity.value(), 1.0);
    EXPECT_DOUBLE_EQ(r.annual_loss.value(), 0.05);
    EXPECT_DOUBLE_EQ(r.vmin.value(), 100.0);
    EXPECT_DOUBLE_EQ(r.vmax.value(), 1000.0);
    EXPECT_DOUBLE_EQ(r.vcost.value(), 5.0);
    EXPECT_DOUBLE_EQ(r.vini.value(), 500.0);
    EXPECT_DOUBLE_EQ(r.vfin.value(), 600.0);
}

TEST_F(TestJsonReservoir, SerializeDeserializeRoundTrip)
{
    nlohmann::json j = test_reservoir;
    Reservoir r = j;
    
    EXPECT_EQ(r.uid, test_reservoir.uid);
    EXPECT_EQ(r.name, test_reservoir.name);
    EXPECT_EQ(r.active, test_reservoir.active);
    EXPECT_EQ(r.junction, test_reservoir.junction);
    EXPECT_DOUBLE_EQ(r.capacity.value(), test_reservoir.capacity.value());
    EXPECT_DOUBLE_EQ(r.annual_loss.value(), test_reservoir.annual_loss.value());
    EXPECT_DOUBLE_EQ(r.vmin.value(), test_reservoir.vmin.value());
    EXPECT_DOUBLE_EQ(r.vmax.value(), test_reservoir.vmax.value());
    EXPECT_DOUBLE_EQ(r.vcost.value(), test_reservoir.vcost.value());
    EXPECT_DOUBLE_EQ(r.vini.value(), test_reservoir.vini.value());
    EXPECT_DOUBLE_EQ(r.vfin.value(), test_reservoir.vfin.value());
}

} // namespace gtopt
