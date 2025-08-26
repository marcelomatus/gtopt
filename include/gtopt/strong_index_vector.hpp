/**
 * @file      strong_index_vector.hpp
 * @brief     Header for type-safe vector access with strong typing
 * @date      Sun Mar 23 22:13:51 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides a type-safe vector implementation that requires
 * strongly-typed indices for element access, preventing accidental
 * access with incorrect index types. The implementation leverages
 * move semantics for efficient memory management and operations.
 */

#pragma once

#include <cassert>
#include <vector>

namespace gtopt
{

/**
 * A vector that enforces access through a strongly-typed index.
 * Inherits privately from std::vector to ensure type safety.
 * Implements efficient move operations marked with noexcept to
 * enable compiler optimizations.
 */
template<typename Index, typename T>
class StrongIndexVector : private std::vector<T>
{
public:
  // Standard constructors with appropriate move semantics
  constexpr StrongIndexVector() noexcept = default;

  explicit constexpr StrongIndexVector(typename std::vector<T>::size_type count)
      : std::vector<T>(count)
  {
  }

  explicit constexpr StrongIndexVector(typename std::vector<T>::size_type count,
                                       const T& value)
      : std::vector<T>(count, value)
  {
  }

  template<class InputIt>
  constexpr StrongIndexVector(InputIt first, InputIt last)
      : std::vector<T>(first, last)
  {
  }

  // Uses initializer list to construct vector
  constexpr StrongIndexVector(std::initializer_list<T> init)
      : std::vector<T>(init)
  {
  }

  // Special member functions
  constexpr StrongIndexVector(StrongIndexVector const& other) = default;
  constexpr StrongIndexVector(StrongIndexVector&& other) noexcept = default;
  constexpr ~StrongIndexVector() = default;

  [[nodiscard]] constexpr auto& operator[](Index pos) noexcept
  {
    return std::vector<T>::operator[](pos.value_of());
  }

  [[nodiscard]] constexpr auto& operator[](Index pos) const noexcept
  {
    return std::vector<T>::operator[](pos.value_of());
  }

  [[nodiscard]] constexpr auto& at(Index pos) const
  {
    return std::vector<T>::at(pos.value_of());
  }

  [[nodiscard]] constexpr auto& at(Index pos)
  {
    return std::vector<T>::at(pos.value_of());
  }

  // Type aliases
  using typename std::vector<T>::value_type;
  using typename std::vector<T>::allocator_type;
  using typename std::vector<T>::size_type;
  using typename std::vector<T>::difference_type;
  using typename std::vector<T>::reference;
  using typename std::vector<T>::const_reference;
  using typename std::vector<T>::pointer;
  using typename std::vector<T>::const_pointer;
  using typename std::vector<T>::iterator;
  using typename std::vector<T>::const_iterator;
  using typename std::vector<T>::reverse_iterator;
  using typename std::vector<T>::const_reverse_iterator;

  // Assignment operators
  constexpr StrongIndexVector& operator=(StrongIndexVector const& other) =
      default;
  constexpr StrongIndexVector& operator=(StrongIndexVector&& other) noexcept =
      default;
  using std::vector<T>::operator=;

  // Inherited functions with constexpr and noexcept where applicable
  using std::vector<T>::assign;
  using std::vector<T>::get_allocator;
  using std::vector<T>::front;
  using std::vector<T>::back;
  using std::vector<T>::data;
  using std::vector<T>::begin;
  using std::vector<T>::cbegin;
  using std::vector<T>::end;
  using std::vector<T>::cend;
  using std::vector<T>::rbegin;
  using std::vector<T>::crbegin;
  using std::vector<T>::rend;
  using std::vector<T>::crend;
  using std::vector<T>::empty;
  using std::vector<T>::size;
  using std::vector<T>::max_size;
  using std::vector<T>::reserve;
  using std::vector<T>::capacity;
  using std::vector<T>::shrink_to_fit;
  using std::vector<T>::clear;
  using std::vector<T>::insert;
  using std::vector<T>::emplace;
  using std::vector<T>::erase;
  using std::vector<T>::push_back;
  using std::vector<T>::emplace_back;
  using std::vector<T>::pop_back;
  using std::vector<T>::resize;
  using std::vector<T>::swap;
};

}  // namespace gtopt
