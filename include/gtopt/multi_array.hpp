/**
 * @file      multi_array.hpp
 * @brief     C++26 replacement for boost::multi_array
 * @date      Sat Feb 09 05:04:00 2026
 * @author    claude
 * @copyright BSD-3-Clause
 *
 * This module provides a simple 2D array implementation using std::vector
 * as a replacement for boost::multi_array with boost::extents.
 */

#pragma once

#include <vector>
#include <stdexcept>

namespace gtopt
{

/**
 * @brief Simple 2D array class using std::vector as storage
 * 
 * This class provides a modern C++26 replacement for boost::multi_array<T, 2>
 * with a simpler interface focused on 2D arrays.
 * 
 * @tparam T The element type
 */
template<typename T>
class multi_array_2d
{
public:
  using value_type = T;
  using size_type = std::size_t;
  using reference = T&;
  using const_reference = const T&;

  /**
   * @brief Default constructor - creates empty array
   */
  multi_array_2d() = default;

  /**
   * @brief Constructor with dimensions
   * @param dim1 First dimension size
   * @param dim2 Second dimension size
   */
  multi_array_2d(size_type dim1, size_type dim2)
      : dim1_(dim1), dim2_(dim2), data_(dim1 * dim2)
  {
  }

  /**
   * @brief Access element at [i][j]
   * @param i First dimension index
   * @return Proxy object that allows chained indexing
   */
  auto operator[](size_type i) noexcept
  {
    return row_proxy {this, i};
  }

  /**
   * @brief Access element at [i][j] (const version)
   * @param i First dimension index
   * @return Const proxy object that allows chained indexing
   */
  auto operator[](size_type i) const noexcept
  {
    return const_row_proxy {this, i};
  }

  /**
   * @brief Get first dimension size
   */
  [[nodiscard]] size_type size1() const noexcept { return dim1_; }

  /**
   * @brief Get second dimension size
   */
  [[nodiscard]] size_type size2() const noexcept { return dim2_; }

  /**
   * @brief Get total number of elements
   */
  [[nodiscard]] size_type size() const noexcept { return data_.size(); }

  /**
   * @brief Check if array is empty
   */
  [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

private:
  size_type dim1_ {0};
  size_type dim2_ {0};
  std::vector<T> data_;

  // Helper proxy class for operator[][]
  struct row_proxy
  {
    multi_array_2d* array;
    size_type row;

    reference operator[](size_type col) noexcept
    {
      return array->data_[row * array->dim2_ + col];
    }
  };

  struct const_row_proxy
  {
    const multi_array_2d* array;
    size_type row;

    const_reference operator[](size_type col) const noexcept
    {
      return array->data_[row * array->dim2_ + col];
    }
  };
};

}  // namespace gtopt
