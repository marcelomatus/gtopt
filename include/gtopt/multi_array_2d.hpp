/**
 * @file      multi_array_2d.hpp
 * @brief     Minimal 2D array wrapper around std::vector
 * @date      Sat Feb 08 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a minimal replacement for boost::multi_array with 2 dimensions.
 * This is a simple wrapper around std::vector that provides 2D array semantics.
 */

#pragma once

#include <vector>

namespace gtopt
{

/**
 * @class MultiArray2D
 * @brief Minimal 2D array wrapper around std::vector
 * 
 * Provides a simple 2D array interface compatible with boost::multi_array usage
 * in the gtopt codebase. Supports:
 * - 2D indexing via operator[]
 * - empty() to check initialization
 * - Default construction (empty state)
 * - Construction with dimensions
 * 
 * @tparam T Element type
 */
template<typename T>
class MultiArray2D
{
public:
  /**
   * @brief Default constructor - creates empty array
   */
  MultiArray2D() = default;

  /**
   * @brief Construct with dimensions
   * @param dim1 First dimension size
   * @param dim2 Second dimension size
   */
  MultiArray2D(size_t dim1, size_t dim2)
      : dim1_(dim1)
      , dim2_(dim2)
      , data_(dim1 * dim2)
  {
  }

  /**
   * @brief Check if array is empty (uninitialized)
   */
  [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

  /**
   * @brief Get first dimension size
   */
  [[nodiscard]] size_t size() const noexcept { return dim1_; }

  /**
   * @brief Proxy class for second dimension access
   */
  class Row
  {
  public:
    explicit Row(T* data) : data_(data) {}

    /**
     * @brief Access element in row
     * @note No bounds checking is performed. Callers must ensure j < dim2_.
     */
    T& operator[](size_t j) { return data_[j]; }
    const T& operator[](size_t j) const { return data_[j]; }

  private:
    T* data_;
  };

  /**
   * @brief Const proxy class for second dimension access
   */
  class ConstRow
  {
  public:
    explicit ConstRow(const T* data) : data_(data) {}

    /**
     * @brief Access element in row (const)
     * @note No bounds checking is performed. Callers must ensure j < dim2_.
     */
    const T& operator[](size_t j) const { return data_[j]; }

  private:
    const T* data_;
  };

  /**
   * @brief 2D indexing - first dimension access
   * @note No bounds checking is performed. Callers must ensure i < dim1_.
   */
  Row operator[](size_t i) { return Row(data_.data() + i * dim2_); }

  /**
   * @brief 2D indexing - first dimension access (const)
   * @note No bounds checking is performed. Callers must ensure i < dim1_.
   */
  ConstRow operator[](size_t i) const
  {
    return ConstRow(data_.data() + i * dim2_);
  }

private:
  size_t dim1_ = 0;
  size_t dim2_ = 0;
  std::vector<T> data_;
};

}  // namespace gtopt
