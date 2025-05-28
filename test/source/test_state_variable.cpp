#include <doctest/doctest.h>
#include <gtopt/state_variable.hpp>

using namespace gtopt;

TEST_CASE("StateVariable default construction")
{
  StateVariable var;
  CHECK(var.name().empty());
  CHECK(var.phase_index() == PhaseIndex {unknown_index});
  CHECK(var.first_col() == unknown_index);
  CHECK(var.last_col() == unknown_index);
}

TEST_CASE("StateVariable parameterized construction")
{
  SUBCASE("Valid construction")
  {
    StateVariable var("test_var", PhaseIndex {1}, 10, 20);

    CHECK(var.name() == "test_var");
    CHECK(var.phase_index() == PhaseIndex {1});
    CHECK(var.first_col() == 10);
    CHECK(var.last_col() == 20);
  }

  SUBCASE("Empty name")
  {
    StateVariable var("", PhaseIndex {1}, 0, 1);
    CHECK(var.name().empty());
  }
}

TEST_CASE("StateVariable key generation")
{
  StateVariable var("test", PhaseIndex {3}, 0, 1);
  auto key = var.key();

  CHECK(std::get<0>(key) == "test");
  CHECK(std::get<1>(key) == PhaseIndex {3});
}

TEST_CASE("StateVariable move semantics")
{
  SUBCASE("Move constructed from valid")
  {
    StateVariable var1("original", PhaseIndex {1}, 5, 10);
    StateVariable var2 = std::move(var1);

    CHECK(var2.name() == "original");
    CHECK(var2.first_col() == 5);
    CHECK(var2.last_col() == 10);

    // Original should be in valid but unspecified state
    CHECK(var1.name().empty());
  }

  SUBCASE("Move constructed from default")
  {
    StateVariable var1;
    StateVariable var2 = std::move(var1);

    CHECK(var2.name().empty());
    CHECK(var2.first_col() == unknown_index);
    CHECK(var2.last_col() == unknown_index);
  }
}

TEST_CASE("StateVariable copy semantics")
{
  SUBCASE("Copy constructed from valid")
  {
    StateVariable var1("original", PhaseIndex {1}, 5, 10);
    StateVariable var2 = var1;  // NOLINT

    CHECK(var2.name() == "original");
    CHECK(var2.first_col() == 5);
    CHECK(var2.last_col() == 10);

    // Original should remain unchanged
    CHECK(var1.name() == "original");
    CHECK(var1.first_col() == 5);
    CHECK(var1.last_col() == 10);
  }

  SUBCASE("Copy constructed from default")
  {
    StateVariable var1;
    StateVariable var2 = var1;  // NOLINT

    CHECK(var2.name().empty());
    CHECK(var2.first_col() == unknown_index);
    CHECK(var2.last_col() == unknown_index);
  }
}

TEST_CASE("StateVariable equality comparison")
{
  StateVariable var1("same", PhaseIndex {1}, 5, 10);
  StateVariable var2("same", PhaseIndex {1}, 5, 10);
  StateVariable var3("different", PhaseIndex {1}, 5, 10);

  CHECK(var1.key() == var2.key());
  CHECK_FALSE(var1.key() == var3.key());

  // Test direct tuple comparison
  auto key1 = var1.key();
  auto key2 = StateVariable::key_t {"same", PhaseIndex {1}};
  CHECK(key1 == key2);
}
