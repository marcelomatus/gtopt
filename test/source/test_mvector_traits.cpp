/**
 * @file      test_mvector_traits.cpp
 * @brief     Unit tests for mvector_traits template classes
 * @date      Mon Jun  2 21:30:23 2025  
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Tests for both mvector_traits and mvector_traits_indexed implementations
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <gtopt/mvector_traits.hpp>

namespace gtopt
{
TEST_CASE("Basic 2D Vector") {
    using traits_2d = mvector_traits<int, std::tuple<std::size_t, std::size_t>>;
    traits_2d::vector_type vec2d = {{1, 2, 3}, {4, 5, 6}};
    
    SUBCASE("Valid access") {
        auto indices = std::make_tuple(std::size_t{1}, std::size_t{2});
        CHECK(traits_2d::at_value(vec2d, indices) == 6);
    }
    
    SUBCASE("Different indices") {
        auto indices = std::make_tuple(std::size_t{0}, std::size_t{1});
        CHECK(traits_2d::at_value(vec2d, indices) == 2);
    }
}

TEST_CASE("Basic 3D Vector") {
    using traits_3d = mvector_traits<int, std::tuple<int, std::size_t, unsigned>>;
    traits_3d::vector_type vec3d = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
    
    SUBCASE("First element") {
        auto indices3d = std::make_tuple(0, std::size_t{1}, 0u);
        CHECK(traits_3d::at_value(vec3d, indices3d) == 3);
    }
    
    SUBCASE("Second element") {
        auto indices3d = std::make_tuple(1, std::size_t{0}, 1u);
        CHECK(traits_3d::at_value(vec3d, indices3d) == 6);
    }
}

TEST_CASE("Mixed Index Types") {
    using traits_mixed = mvector_traits<double, std::tuple<int, short, unsigned long>>;
    traits_mixed::vector_type vec = {
        {{1.1, 2.2}, {3.3, 4.4}},
        {{5.5, 6.6}, {7.7, 8.8}}
    };
    
    auto indices = std::make_tuple(1, short{1}, 0ul);
    CHECK(traits_mixed::at_value(vec, indices) == doctest::Approx(7.7));
}

TEST_CASE("Indexed Basic 2D Vector") {
    using traits_idx = mvector_traits_indexed<int, std::tuple<std::size_t, std::size_t>>;
    traits_idx::vector_type vec = {{1, 2, 3}, {4, 5, 6}};
    
    auto indices = std::make_tuple(std::size_t{0}, std::size_t{2});
    CHECK(traits_idx::at_value(vec, indices) == 3);
}

TEST_CASE("Deep Nesting") {
    using traits_deep = mvector_traits_indexed<float, std::tuple<int, int, int, int>>;
    traits_deep::vector_type vec = {{{{1.1f, 2.2f}, {3.3f, 4.4f}}, 
                                  {{5.5f, 6.6f}, {7.7f, 8.8f}}};
    
    auto indices = std::make_tuple(0, 1, 0, 1);
    CHECK(traits_deep::at_value(vec, indices) == doctest::Approx(4.4f));
}

TEST_CASE("Const Correctness") {
    using traits_const = mvector_traits<const int, std::tuple<int, int>>;
    const traits_const::vector_type vec = {{1, 2}, {3, 4}};
    
    auto indices = std::make_tuple(1, 1);
    CHECK(traits_const::at_value(vec, indices) == 4);
}

TEST_CASE("Empty Vector") {
    using traits_empty = mvector_traits<int, std::tuple<int>>;
    traits_empty::vector_type vec;
    
    // Undefined behavior test - should at least compile
    auto indices = std::make_tuple(0);
    CHECK_NOTHROW(traits_empty::at_value(vec, indices));
}

} // namespace gtopt
