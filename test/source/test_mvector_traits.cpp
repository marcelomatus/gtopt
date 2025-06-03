/**
 * @file      test_mvector_traits.cpp
 * @brief     Unit tests for mvector_traits template classes
 * @date      Mon Jun  2 21:30:23 2025  
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests for both mvector_traits and mvector_traits_indexed implementations
 */

#include <gtest/gtest.h>
#include <gtopt/mvector_traits.hpp>

namespace gtopt
{
namespace
{

TEST(MVectorTraits, Basic2DVector)
{
    using traits_2d = mvector_traits<int, std::tuple<std::size_t, std::size_t>>;
    traits_2d::vector_type vec2d = {{1, 2, 3}, {4, 5, 6}};
    
    // Test valid access
    auto indices = std::make_tuple(std::size_t{1}, std::size_t{2});
    EXPECT_EQ(traits_2d::at_value(vec2d, indices), 6);
    
    // Test different indices
    indices = std::make_tuple(std::size_t{0}, std::size_t{1});
    EXPECT_EQ(traits_2d::at_value(vec2d, indices), 2);
}

TEST(MVectorTraits, Basic3DVector)
{
    using traits_3d = mvector_traits<int, std::tuple<int, std::size_t, unsigned>>;
    traits_3d::vector_type vec3d = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
    
    auto indices3d = std::make_tuple(0, std::size_t{1}, 0u);
    EXPECT_EQ(traits_3d::at_value(vec3d, indices3d), 3);
    
    indices3d = std::make_tuple(1, std::size_t{0}, 1u);
    EXPECT_EQ(traits_3d::at_value(vec3d, indices3d), 6);
}

TEST(MVectorTraits, MixedIndexTypes)
{
    using traits_mixed = mvector_traits<double, std::tuple<int, short, unsigned long>>;
    traits_mixed::vector_type vec = {
        {{1.1, 2.2}, {3.3, 4.4}},
        {{5.5, 6.6}, {7.7, 8.8}}
    };
    
    auto indices = std::make_tuple(1, short{1}, 0ul);
    EXPECT_DOUBLE_EQ(traits_mixed::at_value(vec, indices), 7.7);
}

TEST(MVectorTraitsIndexed, Basic2DVector)
{
    using traits_idx = mvector_traits_indexed<int, std::tuple<std::size_t, std::size_t>>;
    traits_idx::vector_type vec = {{1, 2, 3}, {4, 5, 6}};
    
    auto indices = std::make_tuple(std::size_t{0}, std::size_t{2});
    EXPECT_EQ(traits_idx::at_value(vec, indices), 3);
}

TEST(MVectorTraitsIndexed, DeepNesting)
{
    using traits_deep = mvector_traits_indexed<float, std::tuple<int, int, int, int>>;
    traits_deep::vector_type vec = {{{{1.1f, 2.2f}, {3.3f, 4.4f}}, 
                                  {{5.5f, 6.6f}, {7.7f, 8.8f}}};
    
    auto indices = std::make_tuple(0, 1, 0, 1);
    EXPECT_FLOAT_EQ(traits_deep::at_value(vec, indices), 4.4f);
}

TEST(MVectorTraits, ConstCorrectness)
{
    using traits_const = mvector_traits<const int, std::tuple<int, int>>;
    const traits_const::vector_type vec = {{1, 2}, {3, 4}};
    
    auto indices = std::make_tuple(1, 1);
    EXPECT_EQ(traits_const::at_value(vec, indices), 4);
}

TEST(MVectorTraits, EmptyVector)
{
    using traits_empty = mvector_traits<int, std::tuple<int>>;
    traits_empty::vector_type vec;
    
    // Undefined behavior test - should at least compile
    auto indices = std::make_tuple(0);
    EXPECT_NO_THROW(traits_empty::at_value(vec, indices));
}

} // namespace
} // namespace gtopt
