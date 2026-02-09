/**
 * @file      fmap.hpp
 * @brief     Header of flat_map
 * @date      Sun Mar 23 16:26:44 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the flat_map using modern C++ (C++23/26).
 * When std::flat_map is available, it is used directly.
 * Otherwise, a standards-compatible sorted vector implementation is provided.
 */

#pragma once

#ifdef GTOPT_USE_UNORDERED_MAP

#  include <cstddef>
#  include <functional>
#  include <tuple>
#  include <unordered_map>
#  include <utility>

namespace gtopt
{

namespace detail
{

inline auto hash_combine(std::size_t seed, std::size_t value) -> std::size_t
{
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

template<typename T>
auto hash_single(const T& v) -> std::size_t
{
  return std::hash<std::decay_t<T>> {}(v);
}

template<typename Tuple, std::size_t... Is>
auto hash_tuple_impl(const Tuple& t, std::index_sequence<Is...>)
    -> std::size_t
{
  std::size_t seed = 0;
  ((seed = hash_combine(seed, hash_single(std::get<Is>(t)))), ...);
  return seed;
}

}  // namespace detail

struct tuple_hash
{
  template<typename... Ts>
  auto operator()(const std::tuple<Ts...>& key) const -> std::size_t
  {
    return detail::hash_tuple_impl(key, std::index_sequence_for<Ts...> {});
  }

  template<typename T1, typename T2>
  auto operator()(const std::pair<T1, T2>& key) const -> std::size_t
  {
    std::size_t seed = detail::hash_single(key.first);
    return detail::hash_combine(seed, detail::hash_single(key.second));
  }

  template<typename T>
    requires requires(T t) { std::hash<T> {}(t); }
  auto operator()(const T& key) const -> std::size_t
  {
    return std::hash<T> {}(key);
  }
};

using hash_type = tuple_hash;

template<typename key_type, typename value_type>
using flat_map = std::unordered_map<key_type, value_type, hash_type>;

}  // namespace gtopt

#else

#  if __has_include(<flat_map>)
#    include <flat_map>
#  endif

#  if defined(__cpp_lib_flat_map)

namespace gtopt
{

template<typename key_type, typename value_type>
using flat_map = std::flat_map<key_type, value_type>;

}  // namespace gtopt

#  else

#    include <algorithm>
#    include <cstddef>
#    include <functional>
#    include <utility>
#    include <vector>

namespace gtopt
{

/// Sorted-vector-based flat_map polyfill for compilers without std::flat_map
template<typename Key, typename Value, typename Compare = std::less<Key>>
class flat_map
{
public:
  using key_type = Key;
  using mapped_type = Value;
  using value_type = std::pair<Key, Value>;
  using size_type = std::size_t;
  using iterator = typename std::vector<value_type>::iterator;
  using const_iterator = typename std::vector<value_type>::const_iterator;

  flat_map() = default;

  auto begin() noexcept -> iterator { return data_.begin(); }
  auto end() noexcept -> iterator { return data_.end(); }
  auto begin() const noexcept -> const_iterator { return data_.begin(); }
  auto end() const noexcept -> const_iterator { return data_.end(); }

  [[nodiscard]] auto empty() const noexcept -> bool { return data_.empty(); }
  [[nodiscard]] auto size() const noexcept -> size_type
  {
    return data_.size();
  }

  void clear() noexcept { data_.clear(); }

  auto operator[](const Key& key) -> Value&
  {
    auto it = lower_bound_impl(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return it->second;
    }
    it = data_.insert(
        it, value_type(std::piecewise_construct, std::forward_as_tuple(key),
                       std::tuple<>()));
    return it->second;
  }

  auto find(const Key& key) -> iterator
  {
    auto it = lower_bound_impl(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return it;
    }
    return data_.end();
  }

  auto find(const Key& key) const -> const_iterator
  {
    auto it = lower_bound_impl(key);
    if (it != data_.end() && !comp_(key, it->first)) {
      return it;
    }
    return data_.end();
  }

  template<typename... Args>
  auto emplace(Args&&... args) -> std::pair<iterator, bool>
  {
    value_type val(std::forward<Args>(args)...);
    auto it = lower_bound_impl(val.first);
    if (it != data_.end() && !comp_(val.first, it->first)) {
      return {it, false};
    }
    it = data_.insert(it, std::move(val));
    return {it, true};
  }

  auto erase(const Key& key) -> size_type
  {
    auto it = find(key);
    if (it != data_.end()) {
      data_.erase(it);
      return 1;
    }
    return 0;
  }

  [[nodiscard]] auto count(const Key& key) const -> size_type
  {
    return find(key) != data_.end() ? 1 : 0;
  }

  [[nodiscard]] auto contains(const Key& key) const -> bool
  {
    return find(key) != data_.end();
  }

private:
  auto lower_bound_impl(const Key& key) -> iterator
  {
    return std::lower_bound(
        data_.begin(), data_.end(), key,
        [this](const value_type& p, const Key& k) {
          return comp_(p.first, k);
        });
  }

  auto lower_bound_impl(const Key& key) const -> const_iterator
  {
    return std::lower_bound(
        data_.begin(), data_.end(), key,
        [this](const value_type& p, const Key& k) {
          return comp_(p.first, k);
        });
  }

  std::vector<value_type> data_;
  [[no_unique_address]] Compare comp_ {};
};

}  // namespace gtopt

#  endif

#endif
