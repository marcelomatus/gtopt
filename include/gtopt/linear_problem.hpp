/**
 * @file      linear_problem.hpp
 * @brief     Header of
 * @date      Sun Mar 23 14:50:34 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtopt/fmap.hpp>

namespace gtopt
{

constexpr double const CoinDblMax = std::numeric_limits<double>::max();

using SparseVector = gtopt::flat_map<size_t, double>;

using SparseMatrix = std::vector<SparseVector>;

struct SparseRow
{
  using cmap_t = gtopt::flat_map<size_t, double>;

  std::string name;
  double lowb {0};
  double uppb {0};
  cmap_t cmap {};

  constexpr SparseRow& bound(const double lb, const double ub)
  {
    lowb = lb;
    uppb = ub;
    return *this;
  }
  constexpr SparseRow& less_equal(const double ub)
  {
    return bound(-CoinDblMax, ub);
  }
  constexpr SparseRow& greater_equal(const double lb)
  {
    return bound(lb, CoinDblMax);
  }
  constexpr SparseRow& equal(const double rhs = 0) { return bound(rhs, rhs); }

  [[nodiscard]] auto get_coeff(const size_t key) const
  {
    auto&& iter = cmap.find(key);
    return (iter != cmap.end()) ? iter->second : 0.0;
  }
  auto& set_coeff(const size_t c, const double e) { return cmap[c] = e; }

  auto& operator[](const size_t key) { return cmap[key]; }
  auto operator[](const size_t key) const { return get_coeff(key); }

  [[nodiscard]] auto size() const { return cmap.size(); }
  void reserve(size_t n) { cmap.reserve(n); }

  template<typename Int = size_t,
           typename Dbl = double,
           typename KVec = std::vector<Int>,
           typename VVec = std::vector<Dbl>>
  auto to_flat(const double eps = 0.0) const -> std::pair<KVec, VVec>
  {
    using key_t = typename KVec::value_type;
    using value_t = typename VVec::value_type;

    const auto msize = cmap.size();
    KVec keys;
    keys.reserve(msize);
    VVec vals;
    vals.reserve(msize);

    for (auto&& [key, value] : cmap) {
      if (std::abs(value) > eps) {
        keys.push_back(static_cast<key_t>(key));
        vals.push_back(static_cast<value_t>(value));
      }
    }

    return {std::move(keys), std::move(vals)};
  }
};

struct SparseCol
{
  std::string name;
  double lowb {0};
  double uppb {CoinDblMax};
  double cost {0};
  bool is_integer {false};

  constexpr SparseCol& equal(const double value)
  {
    lowb = value;
    uppb = value;
    return *this;
  }

  constexpr SparseCol& free()
  {
    lowb = -CoinDblMax;
    uppb = CoinDblMax;
    return *this;
  }

  constexpr SparseCol& integer()
  {
    is_integer = true;
    return *this;
  }
};

struct FlatLinearProblem
{
  using index_t = int;
  using name_vec_t = std::vector<std::string>;
  using index_map_t = std::unordered_map<std::string_view, index_t>;

  index_t ncols {};
  index_t nrows {};

  std::vector<index_t> matbeg;
  std::vector<index_t> matind;
  std::vector<double> matval;
  std::vector<double> collb;
  std::vector<double> colub;
  std::vector<double> objval;
  std::vector<double> rowlb;
  std::vector<double> rowub;
  std::vector<index_t> colint;

  name_vec_t colnm;
  name_vec_t rownm;
  index_map_t colmp;
  index_map_t rowmp;

  std::string name;
};

struct FlatOptions
{
  constexpr static auto default_reserve_factor = 1.25;

  double eps {0};  // if negative, don't check
  bool col_with_names {false};
  bool row_with_names {false};
  bool col_with_name_map {false};
  bool row_with_name_map {false};
  bool move_names {true};
  bool reserve_matrix {true};
  double reserve_factor {default_reserve_factor};
};

class LinearProblem
{
public:
  using index_t = size_t;

  constexpr static auto default_reserve_size = 1024;

  [[nodiscard]] explicit LinearProblem(std::string name = {},
                                       size_t rsize = default_reserve_size);

  index_t add_col(SparseCol&& col)
  {
    const index_t index = cols.size();

    if (col.is_integer) {
      ++colints;
    }

    cols.emplace_back(std::move(col));
    return index;
  }

  index_t add_row(SparseRow&& row)
  {
    const index_t index = rows.size();

    ncoeffs += row.size();
    rows.emplace_back(std::move(row));
    return index;
  }

  [[nodiscard]] auto&& col_at(index_t index) { return cols[index]; }
  [[nodiscard]] auto&& col_at(index_t index) const { return cols.at(index); }

  [[nodiscard]] auto get_col_lowb(index_t index) const
  {
    return cols.at(index).lowb;
  }
  [[nodiscard]] auto get_col_uppb(index_t index) const
  {
    return cols.at(index).uppb;
  }

  [[nodiscard]] auto& row_at(index_t index) { return rows[index]; }
  [[nodiscard]] const auto& row_at(index_t index) const
  {
    return rows.at(index);
  }

  [[nodiscard]] index_t get_numrows() const { return rows.size(); }
  [[nodiscard]] index_t get_numcols() const { return cols.size(); }

  void set_coeff(index_t row, index_t col, double coeff, double eps = 0.0);
  [[nodiscard]] double get_coeff(index_t row, index_t col) const;

  [[nodiscard]] FlatLinearProblem to_flat(const FlatOptions& opts = {});

private:
  using cols_t = std::vector<SparseCol>;
  using rows_t = std::vector<SparseRow>;

  std::string pname;
  cols_t cols;
  rows_t rows;
  size_t ncoeffs {};

  size_t colints {};
};

}  // namespace gtopt
