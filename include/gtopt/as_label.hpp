/**
 * @file      as_label.hpp
 * @brief     String label generation utilities
 * @date      Fri May 16 20:16:01 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module provides compile-time string label generation functionality with:
 * - Efficient concatenation of multiple values
 * - Type-safe string conversion
 * - Custom separator support
 * - Move semantics optimization
 *
 * The main interface is the `as_label()` function which:
 * - Accepts any number of arguments of different types
 * - Converts each argument to string representation
 * - Joins them with a configurable separator
 * - Returns a concatenated std::string (case is preserved)
 *
 * Use `lowercase(str)` to produce a lazy zero-copy view that applies
 * per-character lowercasing.  Pass the result directly to `as_label()`
 * to write lowercased text into the output without an intermediate allocation:
 *
 * @code{.cpp}
 * std::string_view cn = "Generator";
 * auto a = as_label(lowercase(cn), 1, 2); // "generator_1_2"
 * @endcode
 *
 * Supported argument types:
 * - std::string and string views
 * - Built-in numeric types (converted via std::format)
 * - Any type convertible to string_view
 * - Any type formattable via std::format
 * - Any char range (e.g. LowercaseView from lowercase())
 *
 * Example usage:
 *
 * @code{.cpp}
 * auto label1 = as_label("prefix", 42, "suffix"); // "prefix_42_suffix"
 * auto label2 = as_label<'-'>("a", "b", "c");    // "a-b-c"
 * auto label3 = as_label(lowercase("Class"), 42); // "class_42"
 * @endcode
 */

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gtopt
{

namespace detail
{

[[nodiscard]] constexpr char to_lower_char(char c) noexcept
{
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

[[nodiscard]] constexpr bool is_ascii_upper(char c) noexcept
{
  return c >= 'A' && c <= 'Z';
}

[[nodiscard]] constexpr bool is_ascii_lower(char c) noexcept
{
  return c >= 'a' && c <= 'z';
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept
{
  return c >= '0' && c <= '9';
}

// True when snake_case conversion should insert an underscore immediately
// before the character at position @p i of @p sv (with @p i >= 1).
//
// Word-boundary rule (standard "HTTP + Response" interpretation):
//   - sv[i] must be an uppercase letter
//   - AND either the previous character is lower/digit (end of a lower run),
//   - OR the previous is upper AND the next (if any) is lower — i.e. this
//     upper letter is the first character of a new word following an
//     acronym.
//
// Examples:
//   "GeneratorLP"    → "generator_lp"
//   "HTTPResponse"   → "http_response"
//   "XMLHttpRequest" → "xml_http_request"
//   "userID"         → "user_id"
//   "Gen2Bus"        → "gen2_bus"
[[nodiscard]] constexpr bool snake_needs_underscore_before(
    std::string_view sv, std::size_t i) noexcept
{
  if (!is_ascii_upper(sv[i])) {
    return false;
  }
  const char prev = sv[i - 1];
  return is_ascii_lower(prev) || is_ascii_digit(prev)
      || (is_ascii_upper(prev) && i + 1 < sv.size()
          && is_ascii_lower(sv[i + 1]));
}

// Single-pass count of the number of characters the snake_case conversion
// of @p sv would produce.  Used to size SnakeCaseView and to reserve() the
// output buffer in the eager snake_case(std::string) overload.
[[nodiscard]] constexpr std::size_t snake_case_size(
    std::string_view sv) noexcept
{
  std::size_t n = 0;
  for (std::size_t i = 0; i < sv.size(); ++i) {
    if (i > 0 && snake_needs_underscore_before(sv, i)) {
      ++n;
    }
    ++n;
  }
  return n;
}

// Improved concept for string-like types
template<typename T>
concept string_like = std::is_convertible_v<const T&, std::string_view>;

// Concept for types that are integral or implicitly convertible to an integral
// type (e.g. strong::type<int, ...> with implicitly_convertible_to<int>).
// This enables the fast std::to_chars path instead of std::format.
template<typename T>
concept integral_convertible = std::integral<T>
    || (std::is_convertible_v<T, std::int64_t> && !std::is_floating_point_v<T>
        && !string_like<T> && !std::is_same_v<std::remove_cvref_t<T>, bool>);

// Concept for input ranges of `char` that are NOT string-like.
// Matches lazy views such as LowercaseView that yield characters on access
// without creating a contiguous buffer.
template<typename T>
concept char_range = std::ranges::input_range<std::remove_cvref_t<T>>
    && std::same_as<std::ranges::range_value_t<std::remove_cvref_t<T>>, char>
    && !string_like<std::remove_cvref_t<T>>;

// Maximum chars needed for a 64-bit signed integer: "-9223372036854775808"
inline constexpr std::size_t int_buf_size = 21;

// Convert an integral value to a string using std::to_chars (no format string
// parsing overhead).  Returns a std::string owning the converted text.
template<integral_convertible T>
[[nodiscard]] inline std::string int_to_string(const T& value)
{
  std::array<char, int_buf_size> buf {};
  const auto ival = static_cast<std::int64_t>(value);
  auto* const begin = buf.data();
  auto* const end = begin
      + buf.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto [ptr, ec] = std::to_chars(begin, end, ival);
  return {begin, ptr};
}

// Compile-time lookup table mapping integers 0–1023 to their string
// representations.  The buffer and index are built entirely at compile
// time so that cached_int_view() returns a std::string_view pointing
// into static storage with zero runtime allocation.

inline constexpr std::size_t int_cache_size = 1024;

// Total chars: 1*10 + 2*90 + 3*900 + 4*24 = 2986, plus 1024 NULs.
// Upper bound: 4 * 1024 + 1024 = 5120 chars is safe.
inline constexpr std::size_t int_cache_buf_len = 5120;

struct IntCacheData
{
  std::array<char, int_cache_buf_len> buf {};
  std::array<std::uint16_t, int_cache_size> offsets {};
  std::array<std::uint8_t, int_cache_size> lengths {};
};

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,misc-const-correctness)
consteval IntCacheData build_int_cache() noexcept
{
  IntCacheData data {};
  std::size_t pos = 0;

  for (std::size_t i = 0; i < int_cache_size; ++i) {
    data.offsets[i] = static_cast<std::uint16_t>(pos);

    // Convert i to decimal digits into the buffer
    if (i == 0) {
      data.buf[pos] = '0';
      data.lengths[i] = 1;
      pos += 2;  // char + NUL
    } else {
      // Write digits in reverse, then flip
      std::size_t start = pos;
      std::size_t val = i;
      while (val > 0) {
        data.buf[pos] = static_cast<char>('0' + (val % 10));
        ++pos;
        val /= 10;
      }
      data.lengths[i] = static_cast<std::uint8_t>(pos - start);
      // Reverse the digits in place
      std::size_t lo = start;
      std::size_t hi = pos - 1;
      while (lo < hi) {
        char tmp = data.buf[lo];
        data.buf[lo] = data.buf[hi];
        data.buf[hi] = tmp;
        ++lo;
        --hi;
      }
      ++pos;  // NUL terminator (already zero-initialized)
    }
  }
  return data;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,misc-const-correctness)

inline constexpr IntCacheData int_cache = build_int_cache();

[[nodiscard]] constexpr std::optional<std::string_view> cached_int_view(
    std::int64_t n) noexcept
{
  if (std::cmp_less(n, 0) || std::cmp_greater_equal(n, int_cache_size))
      [[unlikely]]
  {
    return std::nullopt;
  }
  auto idx = static_cast<std::size_t>(n);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
  return std::string_view(int_cache.buf.data() + int_cache.offsets[idx],
                          int_cache.lengths[idx]);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-bounds-constant-array-index)
}

/// String holder that avoids heap allocation for integral and string_view
/// arguments.  Integer values are formatted into a small stack buffer.
/// Only the std::format fallback path allocates on the heap.
///
/// Uses a tag to track which storage is active, computing the view on
/// demand in view().  This avoids self-referential pointers that would
/// break on move/copy.
class string_holder
{
  enum class Tag : std::uint8_t
  {
    ext,
    buf,
    owned,
  };

  std::array<char, int_buf_size> int_buf_ {};
  std::string owned_;
  std::string_view ext_;
  std::uint8_t int_len_ {0};
  Tag tag_ {Tag::ext};

public:
  // For string-like types — zero allocation
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
  template<string_like T>
    requires(!std::same_as<std::remove_cvref_t<T>, std::string>)
  constexpr explicit string_holder(T&& value) noexcept
      : ext_(std::string_view(std::forward<T>(value)))
  {
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)

  // For const string refs — zero allocation, just view
  constexpr explicit string_holder(const std::string& s) noexcept
      : ext_(s)
  {
  }

  // For rvalue strings — take ownership
  explicit string_holder(std::string&& s) noexcept
      : owned_(std::move(s))
      , tag_(Tag::owned)
  {
  }

  // For single char — store as a 1-character string, not as integer
  constexpr explicit string_holder(char c) noexcept
      : int_buf_ {
            c,
        }
      , int_len_ {1}
      , tag_(Tag::buf)
  {
  }

  // Fast path for integral and integral-convertible types.
  // Uses compile-time cache for 0–1023, stack buffer otherwise.
  template<typename T>
    requires(!string_like<T> && integral_convertible<T>)
  explicit string_holder(const T& value) noexcept
  {
    const auto ival = static_cast<std::int64_t>(value);
    if (auto sv = cached_int_view(ival)) {
      ext_ = *sv;
      tag_ = Tag::ext;
      return;
    }
    tag_ = Tag::buf;
    auto* const begin = int_buf_.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto* const end = begin + int_buf_.size();
    const auto [ptr, ec] = std::to_chars(begin, end, ival);
    int_len_ = static_cast<std::uint8_t>(ptr - begin);
  }

  // Floating-point path — shortest round-trip via std::to_chars.
  // Guarantees a decimal point: 1.0 → "1.0", not "1".
  template<typename T>
    requires std::floating_point<T>
  explicit string_holder(T value) noexcept
      : tag_(Tag::owned)
  {
    // Shortest round-trip for a double needs at most 24 chars
    // (-1.7976931348623157e+308), plus 2 for potential ".0" suffix.
    std::array<char, 26> buf {};
    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    const std::string_view sv(buf.data(), ptr);
    // Ensure a decimal point so the value reads as floating-point.
    // If to_chars produced no '.' and no 'e'/'E', append ".0".
    if (!sv.contains('.') && !sv.contains('e')) {
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      *ptr++ = '.';
      *ptr++ = '0';
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    owned_.assign(buf.data(), ptr);
  }

  // Fallback for non-string, non-integral, non-float types (custom
  // formatters) — allocates via std::format.
  template<typename T>
    requires(!string_like<T> && !integral_convertible<T>
             && !std::floating_point<T> && !char_range<T>)
  explicit string_holder(const T& value)
      : owned_(std::format("{}", value))
      , tag_(Tag::owned)
  {
  }

  // For char ranges (e.g. LowercaseView) — materialises the lazy view into
  // `owned_` so the rest of the as_label machinery can read it as a
  // string_view.  The view itself is not stored; its characters are copied
  // into the local buffer once, eliminating any subsequent re-traversal.
  template<typename T>
    requires char_range<T>
  explicit string_holder(T&& range)
  {
    if constexpr (std::ranges::sized_range<std::remove_cvref_t<T>>) {
      owned_.reserve(std::ranges::size(range));
    }
    std::ranges::copy(std::forward<T>(range), std::back_inserter(owned_));
    tag_ = Tag::owned;
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept
  {
    switch (tag_) {
      case Tag::buf:
        return {int_buf_.data(), int_len_};
      case Tag::owned:
        return owned_;
      case Tag::ext:
        return ext_;
    }
    return {};
  }
};

// Compile-time size calculation
struct label_size
{
  size_t total = 0;
  bool needs_sep = false;

  [[nodiscard]] constexpr label_size add(std::string_view view) const noexcept
  {
    if (view.empty()) [[unlikely]] {
      return *this;
    }
    return {
        .total = total + (needs_sep ? 1 : 0) + view.size(),
        .needs_sep = true,
    };
  }
};

}  // namespace detail

/**
 * @brief Lazy, zero-copy view that lowercases each character on access.
 *
 * Wraps a `std::string_view` and applies `detail::to_lower_char` to every
 * character via a random-access iterator — no heap allocation occurs until the
 * caller copies the characters somewhere (e.g. inside `as_label()`).
 *
 * Because `LowercaseView` satisfies the `detail::char_range` concept it can
 * be passed directly as an argument to `as_label()` and `as_label_into()`:
 *
 * @code{.cpp}
 * std::string_view cn = "Generator";
 * auto a = as_label(lowercase(cn), 1, 2); // "generator_1_2"
 * @endcode
 *
 * @note The view does NOT own the underlying string data.  It must not
 *       outlive the object pointed to by the wrapped `string_view`.
 */
class LowercaseView
{
public:
  // ── Iterator ──────────────────────────────────────────────────────────────
  class iterator
  {
  public:
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    using reference = char;
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept = std::random_access_iterator_tag;

    constexpr iterator() noexcept = default;
    explicit constexpr iterator(const char* p) noexcept
        : ptr_(p)
    {
    }

    [[nodiscard]] constexpr char operator*() const noexcept
    {
      return detail::to_lower_char(*ptr_);
    }

    [[nodiscard]] constexpr char operator[](difference_type n) const noexcept
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return detail::to_lower_char(ptr_[n]);
    }

    constexpr iterator& operator++() noexcept
    {
      ++ptr_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return *this;
    }
    constexpr iterator operator++(int) noexcept
    {
      auto t = *this;
      ++ptr_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return t;
    }
    constexpr iterator& operator--() noexcept
    {
      --ptr_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return *this;
    }
    constexpr iterator operator--(int) noexcept
    {
      auto t = *this;
      --ptr_;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return t;
    }

    constexpr iterator& operator+=(difference_type n) noexcept
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ptr_ += n;
      return *this;
    }
    constexpr iterator& operator-=(difference_type n) noexcept
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ptr_ -= n;
      return *this;
    }

    [[nodiscard]] friend constexpr iterator operator+(
        iterator it, difference_type n) noexcept
    {
      it += n;
      return it;
    }
    [[nodiscard]] friend constexpr iterator operator+(difference_type n,
                                                      iterator it) noexcept
    {
      it += n;
      return it;
    }
    [[nodiscard]] friend constexpr iterator operator-(
        iterator it, difference_type n) noexcept
    {
      it -= n;
      return it;
    }
    [[nodiscard]] friend constexpr difference_type operator-(
        const iterator& a, const iterator& b) noexcept
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return a.ptr_ - b.ptr_;
    }

    [[nodiscard]] constexpr bool operator==(const iterator&) const noexcept =
        default;
    [[nodiscard]] constexpr auto operator<=>(const iterator&) const noexcept =
        default;

  private:
    const char* ptr_ {nullptr};
  };

  // ── LowercaseView interface ───────────────────────────────────────────────
  using value_type = char;
  using size_type = std::size_t;

  constexpr LowercaseView() noexcept = default;
  explicit constexpr LowercaseView(std::string_view sv) noexcept
      : sv_(sv)
  {
  }

  [[nodiscard]] constexpr iterator begin() const noexcept
  {
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    return iterator(sv_.data());
  }
  [[nodiscard]] constexpr iterator end() const noexcept
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return iterator(sv_.data() + sv_.size());
  }

  [[nodiscard]] constexpr size_type size() const noexcept { return sv_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return sv_.empty(); }

  /// Compare element-by-element (after lowercasing) with @p sv.
  [[nodiscard]] bool operator==(std::string_view sv) const noexcept
  {
    return std::ranges::equal(*this, sv);
  }

  /// Materialise the view into a std::string.
  [[nodiscard]] explicit operator std::string() const
  {
    std::string s;
    s.reserve(sv_.size());
    std::ranges::copy(*this, std::back_inserter(s));
    return s;
  }

private:
  std::string_view sv_;
};

static_assert(detail::char_range<LowercaseView>,
              "LowercaseView must satisfy char_range");

/**
 * @brief Lazy, zero-copy view that yields the snake_case form of a string.
 *
 * Wraps a `std::string_view` and emits characters on the fly: every ASCII
 * letter is lowercased (via `detail::to_lower_char`) and underscores are
 * synthesised at word boundaries according to
 * `detail::snake_needs_underscore_before`.  No heap allocation occurs until
 * the caller copies the characters somewhere (e.g. inside `as_label()`).
 *
 * Because `SnakeCaseView` satisfies the `detail::char_range` concept it can
 * be passed directly as an argument to `as_label()` and `as_label_into()`:
 *
 * @code{.cpp}
 * std::string_view cn = "GeneratorLP";
 * auto a = as_label(snake_case(cn), 1, 2); // "generator_lp_1_2"
 * @endcode
 *
 * Word-boundary rule (sibling to Python's inflection library):
 * @code
 *   "GeneratorLP"    → "generator_lp"
 *   "HTTPResponse"   → "http_response"
 *   "XMLHttpRequest" → "xml_http_request"
 *   "userID"         → "user_id"
 *   "Gen2Bus"        → "gen2_bus"
 * @endcode
 *
 * @note The view does NOT own the underlying string data.  It must not
 *       outlive the object pointed to by the wrapped `string_view`.
 */
class SnakeCaseView
{
public:
  // ── Iterator ──────────────────────────────────────────────────────────────
  class iterator
  {
  public:
    using value_type = char;
    using difference_type = std::ptrdiff_t;
    using reference = char;
    using iterator_category = std::forward_iterator_tag;
    using iterator_concept = std::forward_iterator_tag;

    constexpr iterator() noexcept = default;
    explicit constexpr iterator(std::string_view sv,
                                std::size_t idx,
                                bool emit_underscore) noexcept
        : sv_(sv)
        , idx_(idx)
        , emit_underscore_(emit_underscore)
    {
    }

    [[nodiscard]] constexpr char operator*() const noexcept
    {
      if (emit_underscore_) {
        return '_';
      }
      return detail::to_lower_char(sv_[idx_]);
    }

    constexpr iterator& operator++() noexcept
    {
      if (emit_underscore_) {
        // We just emitted a synthesised underscore; the next dereference
        // must return the (lowercased) character at the current index.
        emit_underscore_ = false;
      } else {
        ++idx_;
        if (idx_ < sv_.size()
            && detail::snake_needs_underscore_before(sv_, idx_))
        {
          emit_underscore_ = true;
        }
      }
      return *this;
    }
    constexpr iterator operator++(int) noexcept
    {
      auto t = *this;
      ++(*this);
      return t;
    }

    [[nodiscard]] constexpr bool operator==(const iterator& o) const noexcept
    {
      return idx_ == o.idx_ && emit_underscore_ == o.emit_underscore_;
    }

  private:
    std::string_view sv_;
    std::size_t idx_ {0};
    bool emit_underscore_ {false};
  };

  // ── SnakeCaseView interface ───────────────────────────────────────────────
  using value_type = char;
  using size_type = std::size_t;

  constexpr SnakeCaseView() noexcept = default;
  explicit constexpr SnakeCaseView(std::string_view sv) noexcept
      : sv_(sv)
      , size_(detail::snake_case_size(sv))
  {
  }

  [[nodiscard]] constexpr iterator begin() const noexcept
  {
    return iterator(sv_, 0, /*emit_underscore=*/false);
  }
  [[nodiscard]] constexpr iterator end() const noexcept
  {
    return iterator(sv_, sv_.size(), /*emit_underscore=*/false);
  }

  [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  /// Compare element-by-element (after snake_case conversion) with @p sv.
  [[nodiscard]] bool operator==(std::string_view sv) const noexcept
  {
    return std::ranges::equal(*this, sv);
  }

  /// Materialise the view into a std::string.
  [[nodiscard]] explicit operator std::string() const
  {
    std::string s;
    s.reserve(size_);
    std::ranges::copy(*this, std::back_inserter(s));
    return s;
  }

private:
  std::string_view sv_;
  size_type size_ {0};
};

static_assert(detail::char_range<SnakeCaseView>,
              "SnakeCaseView must satisfy char_range");
static_assert(std::forward_iterator<SnakeCaseView::iterator>,
              "SnakeCaseView::iterator must satisfy forward_iterator");

/**
 * @brief Creates an empty label string
 *
 * @tparam sep Separator character (default '_')
 * @return constexpr std::string Empty string
 *
 * @note This is the base case for empty argument lists
 */
template<char sep = '_'>
[[nodiscard]] constexpr auto as_label() noexcept
{
  return std::string();
}

/**
 * @brief Creates a concatenated label from multiple arguments
 *
 * @tparam sep Separator character between components (default '_')
 * @tparam Args Argument types (automatically deduced)
 * @param args Values to concatenate into label
 * @return constexpr std::string Concatenated label string
 *
 * @throws Nothing if all arguments can be converted to string without throwing
 *
 * @note Arguments are converted to strings in order
 * @note Empty arguments are skipped (no trailing separators)
 * @note The function is constexpr and noexcept when possible
 *
 * Example:
 *
 * @code{.cpp}
 * auto label = as_label("config", "value", 42); // "config_value_42"
 * @endcode
 */
template<char sep = '_', typename... Args>
[[nodiscard]] std::string as_label(Args&&... args) noexcept(
    (std::is_nothrow_constructible_v<detail::string_holder, Args> && ...))
{
  // Create holders for all arguments
  const std::array<detail::string_holder, sizeof...(Args)> holders {
      detail::string_holder(std::forward<Args>(args))...};

  // Calculate total size needed
  detail::label_size size;
  for (const auto& holder : holders) {
    size = size.add(holder.view());
  }

  if (size.total == 0) [[unlikely]] {
    return {};
  }

  // Build the result string
  std::string result;
  result.reserve(size.total);

  bool needs_sep = false;
  for (const auto& holder : holders) {
    const auto view = holder.view();
    if (view.empty()) [[unlikely]] {
      continue;
    }
    if (needs_sep) {
      result.push_back(sep);
    }
    result.append(view);
    needs_sep = true;
  }

  return result;
}

/**
 * @brief Clears and writes a label into an existing string buffer
 *
 * @tparam sep Separator character (default '_')
 * @return constexpr void
 *
 * @note Zero-argument overload — simply clears the buffer
 */
template<char sep = '_'>
constexpr void as_label_into(std::string& result) noexcept
{
  result.clear();
}

/**
 * @brief Writes a concatenated label into an existing string buffer
 *
 * Like as_label(), but reuses the buffer's existing capacity to avoid
 * repeated heap allocations across calls.  The buffer is cleared (not
 * deallocated) on each call, so its capacity grows monotonically to
 * the high-water mark.
 *
 * @tparam sep Separator character between components (default '_')
 * @tparam Args Argument types (automatically deduced)
 * @param result String buffer to write into (cleared, not deallocated)
 * @param args Values to concatenate into label
 */
template<char sep = '_', typename... Args>
void as_label_into(std::string& result, Args&&... args) noexcept(
    (std::is_nothrow_constructible_v<detail::string_holder, Args> && ...))
{
  // Create holders for all arguments
  const std::array<detail::string_holder, sizeof...(Args)> holders {
      detail::string_holder(std::forward<Args>(args))...};

  // Calculate total size needed
  detail::label_size size;
  for (const auto& holder : holders) {
    size = size.add(holder.view());
  }

  result.clear();  // Keeps existing capacity
  if (size.total == 0) [[unlikely]] {
    return;
  }
  result.reserve(size.total);

  // Build the result string
  bool needs_sep = false;
  for (const auto& holder : holders) {
    const auto view = holder.view();
    if (view.empty()) [[unlikely]] {
      continue;
    }
    if (needs_sep) {
      result.push_back(sep);
    }
    result.append(view);
    needs_sep = true;
  }
}

/**
 * @brief Returns a lowercase copy of the given string (rvalue overload).
 *
 * The rvalue string is transformed in-place (zero extra allocation) and
 * returned.  Callers that need a lazy, zero-copy view should pass a
 * `std::string_view` or any non-owning string type to get a `LowercaseView`.
 *
 * @param s An rvalue `std::string`; transformed in-place.
 * @return std::string The same string with every ASCII letter lowercased.
 *
 * Example:
 * @code{.cpp}
 * auto lbl = lowercase(as_label("Generator", 42)); // "generator_42"
 * @endcode
 */
[[nodiscard]] inline std::string lowercase(std::string s) noexcept(
    noexcept(std::ranges::transform(s, s.begin(), detail::to_lower_char)))
{
  std::ranges::transform(s, s.begin(), detail::to_lower_char);
  return s;
}

/**
 * @brief Returns a lazy, zero-copy lowercase view over a string-like value.
 *
 * Unlike the `std::string` overload, this overload does **not** allocate: it
 * wraps the input as a `LowercaseView` that applies `to_lower_char` on each
 * character access.  The view can be passed directly to `as_label()`:
 *
 * @code{.cpp}
 * std::string_view cn = "Generator";
 * auto a = as_label(lowercase(cn), 1, 2); // "generator_1_2"
 * @endcode
 *
 * @param sv Any string-like value (string literal, `std::string_view`,
 *           `LPClassName`, etc.) that is **not** a `std::string` rvalue.
 * @return LowercaseView A lazy view; valid as long as the underlying data
 *         lives (same lifetime rules as `std::string_view`).
 */
template<detail::string_like T>
  requires(!std::same_as<std::remove_cvref_t<T>, std::string>)
[[nodiscard]] constexpr LowercaseView lowercase(T&& sv) noexcept
{
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
  return LowercaseView(std::string_view(std::forward<T>(sv)));
  // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
}

/**
 * @brief Returns a snake_case copy of the given string (std::string overload).
 *
 * Eager counterpart of `snake_case(T&&)`.  Allocates a fresh `std::string`
 * sized to the exact output length (via `detail::snake_case_size`) and
 * writes lowercased characters with synthesised underscores at word
 * boundaries.
 *
 * Unlike `lowercase(std::string)` this overload cannot transform in-place
 * because the output is generally longer than the input (one extra
 * character per inserted underscore).
 *
 * @param s An rvalue `std::string`.
 * @return std::string The snake_case form of @p s.
 *
 * Example:
 * @code{.cpp}
 * auto lbl = snake_case(as_label("HTTPCode")); // "http_code"
 * @endcode
 */
[[nodiscard]] inline std::string snake_case(std::string s)
{
  std::string out;
  out.reserve(detail::snake_case_size(s));
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i > 0 && detail::snake_needs_underscore_before(s, i)) {
      out.push_back('_');
    }
    out.push_back(detail::to_lower_char(s[i]));
  }
  return out;
}

/**
 * @brief Returns a lazy, zero-copy snake_case view over a string-like value.
 *
 * Wraps the input as a `SnakeCaseView` that applies `to_lower_char` and
 * synthesises word-boundary underscores on each character access.  No heap
 * allocation occurs until `as_label()` (or another consumer) copies the
 * characters out.
 *
 * @code{.cpp}
 * std::string_view cn = "GeneratorLP";
 * auto a = as_label(snake_case(cn), 1, 2); // "generator_lp_1_2"
 * @endcode
 *
 * @param sv Any string-like value (string literal, `std::string_view`,
 *           `LPClassName`, etc.) that is **not** a `std::string` rvalue.
 * @return SnakeCaseView A lazy view; valid as long as the underlying data
 *         lives (same lifetime rules as `std::string_view`).
 */
template<detail::string_like T>
  requires(!std::same_as<std::remove_cvref_t<T>, std::string>)
[[nodiscard]] constexpr SnakeCaseView snake_case(T&& sv) noexcept
{
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
  return SnakeCaseView(std::string_view(std::forward<T>(sv)));
  // NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
}

}  // namespace gtopt
