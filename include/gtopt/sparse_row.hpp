#pragma once

#include <limits>
#include <string>
#include <vector>

#include <gtopt/fmap.hpp>

namespace gtopt
{

/**
 * Maximum representable double value used for unbounded constraints
 */
constexpr double const CoinDblMax = std::numeric_limits<double>::max();

/**
 * @class SparseRow 
 * @brief Represents a constraint row in a linear program with sparse coefficients
 */
struct SparseRow
{
  using cmap_t = flat_map<size_t, double>;

  std::string name;  ///< Row/constraint name
  double lowb {0};  ///< Lower bound of the constraint
  double uppb {0};  ///< Upper bound of the constraint
  cmap_t cmap {};  ///< Sparse coefficient map (column index → coeff value)

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

  void reserve(const size_t n) { cmap.reserve(n); }

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

}  // namespace gtopt
