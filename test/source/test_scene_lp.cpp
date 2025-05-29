#include <doctest/doctest.h>
#include <gtopt/scene_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation.hpp>

using namespace gtopt;

TEST_SUITE("SceneLP") {
    TEST_CASE("Default construction") {
        SceneLP scene;
        CHECK(scene.index() == ElementIndex<SceneLP>{unknown_index});
        CHECK(scene.count_scenario() == 0);
        CHECK(scene.first_scenario() == ScenarioIndex{unknown_index});
        CHECK_FALSE(scene.is_active());
    }

    TEST_CASE("Construction with scene and scenarios") {
        Scene scene{
            .uid = 1,
            .active = true,
            .first_scenario = 0,
            .count_scenario = 2
        };

        std::vector<Scenario> scenarios{
            Scenario{.uid = 1, .active = true},
            Scenario{.uid = 2, .active = true},
            Scenario{.uid = 3, .active = false}
        };

        SceneLP scene_lp(scene, scenarios);
        
        CHECK(scene_lp.index() == ElementIndex<SceneLP>{1});
        CHECK(scene_lp.is_active());
        CHECK(scene_lp.first_scenario() == ScenarioIndex{0});
        CHECK(scene_lp.count_scenario() == 2);
    }

    TEST_CASE("Construction with simulation") {
        Scene scene{
            .uid = 2,
            .active = true,
            .first_scenario = 1,
            .count_scenario = 1
        };

        Simulation simulation{
            .scenarios = {
                Scenario{.uid = 1, .active = false},
                Scenario{.uid = 2, .active = true},
                Scenario{.uid = 3, .active = true}
            }
        };

        SceneLP scene_lp(scene, simulation);
        
        CHECK(scene_lp.index() == ElementIndex<SceneLP>{2});
        CHECK(scene_lp.is_active());
        CHECK(scene_lp.first_scenario() == ScenarioIndex{1});
        CHECK(scene_lp.count_scenario() == 1);
    }

    TEST_CASE("Inactive scene") {
        Scene scene{
            .uid = 3,
            .active = false,
            .first_scenario = 0,
            .count_scenario = 2
        };

        std::vector<Scenario> scenarios{
            Scenario{.uid = 1, .active = true},
            Scenario{.uid = 2, .active = true}
        };

        SceneLP scene_lp(scene, scenarios);
        
        CHECK(scene_lp.index() == ElementIndex<SceneLP>{3});
        CHECK_FALSE(scene_lp.is_active());
    }

    TEST_CASE("Edge cases") {
        SUBCASE("Empty scenario list") {
            Scene scene{
                .uid = 4,
                .active = true,
                .first_scenario = 0,
                .count_scenario = 0
            };

            SceneLP scene_lp(scene, std::vector<Scenario>{});
            
            CHECK(scene_lp.count_scenario() == 0);
            CHECK(scene_lp.first_scenario() == ScenarioIndex{unknown_index});
        }

        SUBCASE("Out of bounds scenario range") {
            Scene scene{
                .uid = 5,
                .active = true,
                .first_scenario = 10,  // Invalid index
                .count_scenario = 5
            };

            std::vector<Scenario> scenarios(5);  // Only 5 scenarios
            
            CHECK_THROWS_AS(SceneLP(scene, scenarios), std::out_of_range);
        }
    }
}
