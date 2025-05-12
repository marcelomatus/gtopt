#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

TEST_CASE("StateVariable construction and accessors")
{
  using namespace gtopt;

  // Create test indices
  PhaseIndex phase_idx {0};
  SceneIndex scene_idx {1};
  Index first_col = 10;
  Index last_col = 15;

  // Test standard constructor
  StateVariable var("test_var", scene_idx, phase_idx, first_col, last_col);

  // Test accessors
  CHECK(var.name() == "test_var");
  CHECK(var.phase_index() == phase_idx);
  CHECK(var.scene_index() == scene_idx);
  CHECK(var.first_col() == first_col);
  CHECK(var.last_col() == last_col);
}

TEST_CASE("StateVariable with invalid column ranges")
{
  using namespace gtopt;

  PhaseIndex phase_idx {0};
  SceneIndex scene_idx {1};

  // Test negative column indices
  CHECK_THROWS_AS(StateVariable("negative_first", scene_idx, phase_idx, -1, 5),
                  std::invalid_argument);

  CHECK_THROWS_AS(StateVariable("negative_last", scene_idx, phase_idx, 0, -5),
                  std::invalid_argument);
}

TEST_CASE("StateVariable with single column")
{
  using namespace gtopt;

  PhaseIndex phase_idx {0};
  SceneIndex scene_idx {1};
  Index col = 7;

  // Create a variable that spans only one column
  StateVariable var("single_col", scene_idx, phase_idx, col, col);

  CHECK(var.first_col() == col);
  CHECK(var.last_col() == col);
}

TEST_CASE("StateVariable different phase and scene indices")
{
  using namespace gtopt;

  // Test with various phase and scene indices
  PhaseIndex phase1 {0};
  PhaseIndex phase2 {5};
  SceneIndex scene1 {1};
  SceneIndex scene2 {10};

  StateVariable var1("var1", scene1, phase1, 0, 5);
  StateVariable var2("var2", scene2, phase2, 10, 15);

  CHECK(var1.phase_index() != var2.phase_index());
  CHECK(var1.scene_index() != var2.scene_index());
}

TEST_CASE("StateVariable and map")
{
  using namespace gtopt;

  // Test with various phase and scene indices
  PhaseIndex phase1 {0};
  PhaseIndex phase2 {5};
  SceneIndex scene1 {1};
  SceneIndex scene2 {10};

  StateVariable var1("var1", scene1, phase1, 0, 5);
  StateVariable var2("var2", scene2, phase2, 10, 15);
  state_variable_map_t map;

  state_variable_key_t key1 {
      var1.name(), var1.scene_index(), var1.phase_index()};
  map[key1] = var1;

  CHECK(var1.phase_index() == map[key1].phase_index());
  CHECK(var1.scene_index() == map[key1].scene_index());
  CHECK(var1.name() == map[key1].name());
}
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable default construction") {
    StateVariable var;
    CHECK(var.name().empty());
    CHECK(var.phase_index() == PhaseIndex{});
    CHECK(var.scene_index() == SceneIndex{});
    CHECK(var.first_col() == -1);
    CHECK(var.last_col() == -1);
}

TEST_CASE("StateVariable parameterized construction") {
    SUBCASE("Valid construction") {
        StateVariable var("test_var", SceneIndex{1}, PhaseIndex{2}, 10, 20);
        
        CHECK(var.name() == "test_var");
        CHECK(var.scene_index() == SceneIndex{1});
        CHECK(var.phase_index() == PhaseIndex{2});
        CHECK(var.first_col() == 10);
        CHECK(var.last_col() == 20);
    }

    SUBCASE("Empty name") {
        StateVariable var("", SceneIndex{1}, PhaseIndex{1}, 0, 1);
        CHECK(var.name().empty());
    }
}

TEST_CASE("StateVariable key generation") {
    StateVariable var("test", SceneIndex{3}, PhaseIndex{4}, 0, 1);
    auto key = var.key();
    
    CHECK(std::get<0>(key) == "test");
    CHECK(std::get<1>(key) == SceneIndex{3});
    CHECK(std::get<2>(key) == PhaseIndex{4});
}

TEST_CASE("StateVariable move semantics") {
    StateVariable var1("original", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    StateVariable var2 = std::move(var1);
    
    CHECK(var2.name() == "original");
    CHECK(var2.first_col() == 5);
    CHECK(var2.last_col() == 10);
    
    // Original should be in valid but unspecified state
    CHECK(var1.name().empty());
}

TEST_CASE("StateVariable invalid column indices") {
    SUBCASE("Negative first column") {
        CHECK_THROWS_AS(
            StateVariable("bad", SceneIndex{1}, PhaseIndex{1}, -5, 10),
            std::invalid_argument
        );
    }
    
    SUBCASE("Negative last column") {
        CHECK_THROWS_AS(
            StateVariable("bad", SceneIndex{1}, PhaseIndex{1}, 10, -5),
            std::invalid_argument
        );
    }
    
    SUBCASE("Last column before first") {
        CHECK_THROWS_AS(
            StateVariable("bad", SceneIndex{1}, PhaseIndex{1}, 20, 10),
            std::invalid_argument
        );
    }
    
    SUBCASE("Equal columns") {
        StateVariable var("equal", SceneIndex{1}, PhaseIndex{1}, 5, 5);
        CHECK(var.first_col() == 5);
        CHECK(var.last_col() == 5);
    }
}

TEST_CASE("StateVariable copy semantics") {
    StateVariable var1("original", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    StateVariable var2 = var1;
    
    CHECK(var2.name() == "original");
    CHECK(var2.first_col() == 5);
    CHECK(var2.last_col() == 10);
    
    // Original should remain unchanged
    CHECK(var1.name() == "original");
    CHECK(var1.first_col() == 5);
    CHECK(var1.last_col() == 10);
}

TEST_CASE("StateVariable equality comparison") {
    StateVariable var1("same", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    StateVariable var2("same", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    StateVariable var3("different", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    
    CHECK(var1.key() == var2.key());
    CHECK_FALSE(var1.key() == var3.key());
}

TEST_CASE("StateVariable hash behavior") {
    StateVariable var1("hash_test", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    StateVariable var2("hash_test", SceneIndex{1}, PhaseIndex{1}, 5, 10);
    
    std::hash<state_variable_key_t> hasher;
    CHECK(hasher(var1.key()) == hasher(var2.key()));
}
