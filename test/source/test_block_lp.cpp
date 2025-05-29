#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <gtopt/block_lp.hpp>

using namespace gtopt;

TEST_CASE("BlockLP default construction")
{
    const BlockLP block;
    
    SUBCASE("Default constructed BlockLP has unknown index")
    {
        CHECK(block.index() == BlockIndex{unknown_index});
    }
    
    SUBCASE("Default constructed BlockLP has default Block")
    {
        CHECK(block.uid() == BlockUid{0});
        CHECK(block.duration() == 0);
    }
}

TEST_CASE("BlockLP construction with parameters")
{
    const Block block_data{123, 5.0}; // uid=123, duration=5.0
    const BlockIndex index{42};
    const BlockLP block(block_data, index);
    
    SUBCASE("Constructor properly initializes members")
    {
        CHECK(block.uid() == BlockUid{123});
        CHECK(block.duration() == 5.0);
        CHECK(block.index() == BlockIndex{42});
    }
    
    SUBCASE("Constructor with default index")
    {
        const BlockLP default_index_block(block_data);
        CHECK(default_index_block.index() == BlockIndex{unknown_index});
    }
}

TEST_CASE("BlockLP move semantics")
{
    Block block_data{456, 7.5};
    const BlockIndex index{99};
    BlockLP original(block_data, index);
    
    SUBCASE("Move construction")
    {
        BlockLP moved(std::move(original));
        
        CHECK(moved.uid() == BlockUid{456});
        CHECK(moved.duration() == 7.5);
        CHECK(moved.index() == BlockIndex{99});
    }
    
    SUBCASE("Move assignment")
    {
        BlockLP moved;
        moved = std::move(original);
        
        CHECK(moved.uid() == BlockUid{456});
        CHECK(moved.duration() == 7.5);
        CHECK(moved.index() == BlockIndex{99});
    }
}

TEST_CASE("BlockLP copy semantics")
{
    const Block block_data{789, 3.0};
    const BlockIndex index{13};
    const BlockLP original(block_data, index);
    
    SUBCASE("Copy construction")
    {
        BlockLP copy(original);
        
        CHECK(copy.uid() == original.uid());
        CHECK(copy.duration() == original.duration());
        CHECK(copy.index() == original.index());
    }
    
    SUBCASE("Copy assignment")
    {
        BlockLP copy;
        copy = original;
        
        CHECK(copy.uid() == original.uid());
        CHECK(copy.duration() == original.duration());
        CHECK(copy.index() == original.index());
    }
}

TEST_CASE("BlockLP constexpr usage")
{
    constexpr Block block_data{111, 2.0};
    constexpr BlockIndex index{7};
    constexpr BlockLP block(block_data, index);
    
    SUBCASE("Constexpr construction")
    {
        static_assert(block.uid() == BlockUid{111});
        static_assert(block.duration() == 2.0);
        static_assert(block.index() == BlockIndex{7});
    }
    
    SUBCASE("Constexpr methods")
    {
        constexpr auto uid = block.uid();
        constexpr auto duration = block.duration();
        constexpr auto idx = block.index();
        
        CHECK(uid == BlockUid{111});
        CHECK(duration == 2.0);
        CHECK(idx == BlockIndex{7});
    }
}

TEST_CASE("BlockLP noexcept guarantees")
{
    Block block_data{222, 4.0};
    BlockIndex index{5};
    
    SUBCASE("Constructor is noexcept")
    {
        CHECK(noexcept(BlockLP(block_data, index)));
    }
    
    SUBCASE("Methods are noexcept")
    {
        BlockLP block(block_data, index);
        CHECK(noexcept(block.uid()));
        CHECK(noexcept(block.duration()));
        CHECK(noexcept(block.index()));
    }
    
    SUBCASE("Special members are noexcept")
    {
        CHECK(std::is_nothrow_move_constructible_v<BlockLP>);
        CHECK(std::is_nothrow_move_assignable_v<BlockLP>);
        CHECK(std::is_nothrow_copy_constructible_v<BlockLP>);
        CHECK(std::is_nothrow_copy_assignable_v<BlockLP>);
        CHECK(std::is_nothrow_destructible_v<BlockLP>);
    }
}
