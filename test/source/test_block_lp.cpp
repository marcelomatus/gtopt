#include <doctest/doctest.h>
#include <gtopt/block_lp.hpp>

using namespace gtopt;

namespace
{
}  // namespace

TEST_CASE("BlockLP default construction")
{
  constexpr BlockLP block;

  SUBCASE("Default constructed BlockLP has unknown index")
  {
    CHECK(block.index() == BlockIndex {unknown_index});
  }

  SUBCASE("Default constructed BlockLP has default Block")
  {
    CHECK(block.uid() == BlockUid {unknown_uid});
    CHECK(block.duration() == 0);
  }
}

TEST_CASE("BlockLP construction with parameters")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};
  constexpr BlockLP block(test_block, test_index);

  SUBCASE("Constructor properly initializes members")
  {
    CHECK(block.uid() == BlockUid {test_block.uid});
    CHECK(block.duration() == test_block.duration);
    CHECK(block.index() == test_index);
  }

  SUBCASE("Constructor with default index")
  {
    constexpr BlockLP default_index_block(test_block);
    CHECK(default_index_block.index() == BlockIndex {unknown_index});
  }
}

TEST_CASE("BlockLP move semantics")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  SUBCASE("Move construction")
  {
    BlockLP original(test_block, test_index);
    const BlockLP moved(std::move(original));
    CHECK(moved.uid() == BlockUid {test_block.uid});
    CHECK(moved.index() == test_index);
  }

  SUBCASE("Move assignment")
  {
    BlockLP original(test_block, test_index);
    BlockLP moved;
    moved = std::move(original);
    CHECK(moved.uid() == BlockUid {test_block.uid});
    CHECK(moved.index() == test_index);
  }
}

TEST_CASE("BlockLP copy semantics")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  const BlockLP original(test_block, test_index);

  SUBCASE("Copy construction")
  {
    BlockLP copy(original);  // NOLINT(performance-unnecessary-copy-initialization)
    CHECK(copy.uid() == original.uid());
    CHECK(copy.index() == original.index());
  }

  SUBCASE("Copy assignment")
  {
    BlockLP copy;
    copy = original;
    CHECK(copy.uid() == original.uid());
    CHECK(copy.index() == original.index());
  }
}

TEST_CASE("BlockLP constexpr usage")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  constexpr BlockLP block(test_block, test_index);

  SUBCASE("Constexpr construction")
  {
    static_assert(block.uid() == BlockUid {test_block.uid});
    static_assert(block.index() == test_index);
  }

  SUBCASE("Constexpr methods")
  {
    constexpr auto uid = block.uid();
    constexpr auto idx = block.index();
    CHECK(uid == BlockUid {test_block.uid});
    CHECK(idx == test_index);
  }
}

TEST_CASE("BlockLP noexcept guarantees")
{
  constexpr Block test_block {.uid = 123};
  constexpr BlockIndex test_index {42};

  SUBCASE("Methods are noexcept")
  {
    const BlockLP block(test_block, test_index);
    CHECK(noexcept(block.uid()));
    CHECK(noexcept(block.duration()));
    CHECK(noexcept(block.index()));
  }

  SUBCASE("Special members are noexcept")
  {
    CHECK(std::is_nothrow_move_constructible_v<BlockLP>);
    CHECK(std::is_nothrow_move_assignable_v<BlockLP>);
    CHECK(std::is_nothrow_destructible_v<BlockLP>);
  }
}
