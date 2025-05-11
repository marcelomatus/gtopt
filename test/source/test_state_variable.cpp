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
