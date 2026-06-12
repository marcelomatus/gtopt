#include <cstddef>
#include <stdexcept>

#include <doctest/doctest.h>
#include <gtopt/strong_index_vector.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT
{
struct TestIndex
{
  std::size_t value;

  [[nodiscard]] constexpr std::size_t value_of() const noexcept
  {
    return value;
  }
};

struct TestIndex2
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
  using namespace gtopt;
  StrongIndexVector<TestIndex, int> values(3, 0);

  values[TestIndex {1}] = 42;
  CHECK(values[TestIndex {1}] == 42);
  CHECK(values.at(TestIndex {2}) == 0);
  CHECK_THROWS_AS(auto _ = values.at(TestIndex {3}), std::out_of_range);
}

TEST_CASE("StrongIndexVector initializer list")
{
  const StrongIndexVector<TestIndex, int> values {1, 2, 3};

  CHECK(values[TestIndex {0}] == 1);
  CHECK(values[TestIndex {2}] == 3);
}

TEST_CASE("StrongIndexVector nested 2D access (scene×phase pattern)")
{
  // Mirrors the pattern used by PlanningLP::create_systems, where the
  // build buffer is
  //   StrongIndexVector<SceneIndex,
  //                     StrongIndexVector<PhaseIndex, optional<SystemLP>>>.
  // The inner strong type differs from the outer so that accidental
  // swaps (indexing the outer with a PhaseIndex, or vice versa) fail at
  // compile time instead of silently reading the wrong cell.
  using Row = StrongIndexVector<TestIndex2, int>;
  StrongIndexVector<TestIndex, Row> grid(2);
  for (auto& row : grid) {
    row.resize(3, 0);
  }

  grid[TestIndex {0}][TestIndex2 {0}] = 1;
  grid[TestIndex {0}][TestIndex2 {2}] = 3;
  grid[TestIndex {1}][TestIndex2 {1}] = 99;

  CHECK(grid[TestIndex {0}][TestIndex2 {0}] == 1);
  CHECK(grid[TestIndex {0}][TestIndex2 {1}] == 0);
  CHECK(grid[TestIndex {0}][TestIndex2 {2}] == 3);
  CHECK(grid[TestIndex {1}][TestIndex2 {0}] == 0);
  CHECK(grid[TestIndex {1}][TestIndex2 {1}] == 99);
  CHECK(grid[TestIndex {1}][TestIndex2 {2}] == 0);

  CHECK_THROWS_AS(  // NOLINT(performance-unnecessary-copy-initialization)
      auto _ = grid.at(TestIndex {2}),
      std::out_of_range);
  CHECK_THROWS_AS(auto _ = grid[TestIndex {0}].at(TestIndex2 {3}),
                  std::out_of_range);
}
