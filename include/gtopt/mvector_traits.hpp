/**
 * @file      mvector_traits.hpp
 * @brief     Recursive multi-dimensional vector traits
 * @date      Mon Jun  2 20:54:23 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the mvector_traits template, which recursively nests
 * std::vector types to represent multi-dimensional indexed containers.
 */

#pragma once

#include <tuple>
#include <vector>

namespace gtopt
{

// Forward declaration with default parameter
// template<typename Type, typename UidTuple, std::size_t Depth>
// struct mvector_traits;
template<typename Type,
         typename Tuple,
         std::size_t Depth = std::tuple_size_v<Tuple>>
struct mvector_traits;

// Specialization for depth = 1 (base case)
//
// Note: `vec.at(idx)` (range-checked) instead of `vec[idx]`.  When the
// caller's vector_uid_idx maps (stage_uid, block_uid) to a position
// that exceeds the actual JSON profile length — e.g. PLEXOS multi-day
// runs where the source CSV is daily-pattern and the writer did not
// widen the profile — the previous unchecked `operator[]` read past
// the end and produced uninitialized-memory bounds (observed as
// ``reservezone_drequirement_*`` lower bounds of ``2.83e+256``).
// ``std::out_of_range`` is caught upstream in
// ``input_traits.hpp::optval_sched`` and returned as ``std::nullopt``,
// so callers see a missing-data signal instead of trash.
template<typename Type, typename Tuple>
struct mvector_traits<Type, Tuple, 1U>
{
  using value_type = Type;
  using vector_type = std::vector<Type>;
  using tuple_type = Tuple;

  template<typename Container>
  constexpr static auto at_value(const Container& vec, const tuple_type& uids)
  {
    constexpr std::size_t last_index = std::tuple_size_v<Tuple> - 1;
    return vec.at(std::get<last_index>(uids));
  }
};

// General template for depth > 1
template<typename Type, typename Tuple, std::size_t Depth>
struct mvector_traits
{
  static_assert(Depth > 1,
                "Depth must be greater than 1 for the general template");
  static_assert(Depth <= std::tuple_size_v<Tuple>,
                "Depth cannot exceed tuple size");

  using value_type = Type;
  using vector_type =
      std::vector<typename mvector_traits<Type, Tuple, Depth - 1>::vector_type>;
  using tuple_type = Tuple;

  template<typename Container>
  constexpr static auto at_value(const Container& vec, const tuple_type& uids)
  {
    constexpr std::size_t current_index = std::tuple_size_v<Tuple> - Depth;
    return mvector_traits<Type, Tuple, Depth - 1>::at_value(
        vec.at(std::get<current_index>(uids)), uids);
  }
};

// Convenience alias that automatically deduces the depth
template<typename Type, typename Tuple>
using mvector_traits_auto =
    mvector_traits<Type, Tuple, std::tuple_size_v<Tuple>>;

}  // namespace gtopt
