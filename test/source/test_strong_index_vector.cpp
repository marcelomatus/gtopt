#include <cstddef>
#include <stdexcept>

#include <doctest/doctest.h>
#include <gtopt/strong_index_vector.hpp>

using namespace gtopt;

namespace
{
struct TestIndex
{
  std::size_t value;

  [[nodiscard]] constexpr std::size_t value_of() const noexcept
  {
    return value;
  }
};
}  // namespace

TEST_CASE("StrongIndexVector indexing")
{
  StrongIndexVector<TestIndex, int> values(3, 0);

  values[TestIndex {1}] = 42;
  CHECK(values[TestIndex {1}] == 42);
  CHECK(values.at(TestIndex {2}) == 0);
  CHECK_THROWS_AS(values.at(TestIndex {3}), std::out_of_range);
}

TEST_CASE("StrongIndexVector initializer list")
{
  const StrongIndexVector<TestIndex, int> values {1, 2, 3};

  CHECK(values[TestIndex {0}] == 1);
  CHECK(values[TestIndex {2}] == 3);
}
