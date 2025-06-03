/**
 * @file      mvector_traits.hpp
 * @brief     Header of
 * @date      Mon Jun  2 20:54:23 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <tuple>
#include <utility>
#include <vector>

namespace gtopt
{
template<typename Type, typename UidTuple>
struct mvector_traits
{
};

// Base case: single UID type
template<typename Type, typename Uid>
struct mvector_traits<Type, std::tuple<Uid>>
{
  using value_type = Type;
  using vector_type = std::vector<Type>;
  using uid_tuple_type = std::tuple<Uid>;

  template<typename Container>
  constexpr static auto at_value(const Container& vec,
                                 const uid_tuple_type& uids) noexcept
  {
    return vec[std::get<0>(uids)];
  }
};

// Recursive case: multiple UID types
template<typename Type, typename U1, typename... Uids>
struct mvector_traits<Type, std::tuple<U1, Uids...>>
{
  using value_type = Type;
  using remaining_tuple = std::tuple<Uids...>;
  using vector_type =
      std::vector<typename mvector_traits<Type, remaining_tuple>::vector_type>;
  using uid_tuple_type = std::tuple<U1, Uids...>;

  template<typename Container>
  constexpr static auto at_value(const Container& vec,
                                 const uid_tuple_type& uids) noexcept
  {
    return at_value_impl(
        vec, uids, std::make_index_sequence<sizeof...(Uids)> {});
  }

private:
  template<typename Container, std::size_t... Is>
  constexpr static auto at_value_impl(const Container& vec,
                                      const uid_tuple_type& uids,
                                      std::index_sequence<Is...>) noexcept
  {
    auto remaining_uids = std::make_tuple(std::get<Is + 1>(uids)...);
    return mvector_traits<Type, remaining_tuple>::at_value(
        vec[std::get<0>(uids)], remaining_uids);
  }
};

// Alternative implementation using index-based unfolding for the entire tuple
template<typename Type,
         typename UidTuple,
         std::size_t Depth = std::tuple_size_v<UidTuple>>
struct mvector_traits_indexed
{
};

// Specialization for depth = 1 (base case)
template<typename Type, typename UidTuple>
struct mvector_traits_indexed<Type, UidTuple, 1>
{
  using value_type = Type;
  using vector_type = std::vector<Type>;
  using uid_tuple_type = UidTuple;

  template<typename Container>
  constexpr static auto at_value(const Container& vec,
                                 const uid_tuple_type& uids) noexcept
  {
    return vec[std::get<0>(uids)];
  }
};

// Specialization for depth > 1 (recursive case)
template<typename Type, typename UidTuple, std::size_t Depth>
struct mvector_traits_indexed
{
  using value_type = Type;
  using vector_type = std::vector<
      typename mvector_traits_indexed<Type, UidTuple, Depth - 1>::vector_type>;
  using uid_tuple_type = UidTuple;

  template<typename Container>
  constexpr static auto at_value(const Container& vec,
                                 const uid_tuple_type& uids) noexcept
  {
    constexpr std::size_t current_index = std::tuple_size_v<UidTuple> - Depth;
    return mvector_traits_indexed<Type, UidTuple, Depth - 1>::at_value(
        vec[std::get<current_index>(uids)], uids);
  }
};

}  // namespace gtopt

// Usage examples:
/*
// 2D vector example
using traits_2d = mvector_traits<int, std::tuple<std::size_t, std::size_t>>;
traits_2d::vector_type vec2d = {{1, 2, 3}, {4, 5, 6}};
auto indices = std::make_tuple(std::size_t{1}, std::size_t{2});
auto value = traits_2d::at_value(vec2d, indices); // Returns 6

// 3D vector example with different index types
using traits_3d = mvector_traits<int, std::tuple<int, std::size_t, unsigned>>;
traits_3d::vector_type vec3d = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
auto indices3d = std::make_tuple(0, std::size_t{1}, 0u);
auto value3d = traits_3d::at_value(vec3d, indices3d); // Returns 3

// Using the indexed version
using traits_indexed = mvector_traits_indexed<int, std::tuple<std::size_t,
std::size_t>>; traits_indexed::vector_type vec_idx = {{1, 2, 3}, {4, 5, 6}};
auto indices_idx = std::make_tuple(std::size_t{0}, std::size_t{2});
auto value_idx = traits_indexed::at_value(vec_idx, indices_idx); // Returns 3
*/
