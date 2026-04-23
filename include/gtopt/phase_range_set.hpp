/**
 * @file      phase_range_set.hpp
 * @brief     Parse and query phase range expressions for relaxed UC binaries
 * @date      2026-04-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Supports expressions like:
 *   "all"         — every phase is included
 *   "none"        — no phase is included (default)
 *   "0"           — only phase 0
 *   "1,3,5"       — phases 1, 3, 5
 *   "1:3"         — phases 1, 2, 3 (inclusive)
 *   "3:"          — phases 3 and above
 *   ":5"          — phases 0 through 5
 *   "0,3:5,8:"    — union of individual indices and ranges
 */

#pragma once

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <vector>

namespace gtopt
{

class PhaseRangeSet
{
public:
  /// Construct from a range expression string.
  /// Empty string or "none" → nothing included; "all" → everything included.
  explicit PhaseRangeSet(std::string_view expr = "none") { parse(expr); }

  /// Check whether a given phase index is in the set.
  [[nodiscard]] bool contains(int phase) const noexcept
  {
    if (m_all_) {
      return true;
    }
    return std::ranges::any_of(m_ranges_,
                               [phase](const auto& r)
                               { return phase >= r.lo && phase <= r.hi; });
  }

  /// True if the set matches all phases.
  [[nodiscard]] bool is_all() const noexcept { return m_all_; }

  /// True if the set is empty (matches no phase).
  [[nodiscard]] bool is_none() const noexcept
  {
    return !m_all_ && m_ranges_.empty();
  }

private:
  struct Range
  {
    int lo {};
    int hi {};
  };

  bool m_all_ {false};
  std::vector<Range> m_ranges_;

  void parse(std::string_view expr)
  {
    // Trim whitespace
    while (!expr.empty() && expr.front() == ' ') {
      expr.remove_prefix(1);
    }
    while (!expr.empty() && expr.back() == ' ') {
      expr.remove_suffix(1);
    }

    if (expr.empty() || expr == "none") {
      return;
    }
    if (expr == "all") {
      m_all_ = true;
      return;
    }

    // Split on ',' and parse each token
    while (!expr.empty()) {
      auto comma = expr.find(',');
      auto token =
          (comma == std::string_view::npos) ? expr : expr.substr(0, comma);

      // Trim token
      while (!token.empty() && token.front() == ' ') {
        token.remove_prefix(1);
      }
      while (!token.empty() && token.back() == ' ') {
        token.remove_suffix(1);
      }

      if (!token.empty()) {
        parse_token(token);
      }

      if (comma == std::string_view::npos) {
        break;
      }
      expr.remove_prefix(comma + 1);
    }
  }

  void parse_token(std::string_view token)
  {
    static constexpr int max_phase = std::numeric_limits<int>::max();
    auto colon = token.find(':');

    if (colon == std::string_view::npos) {
      // Single index: "3"
      if (auto val = parse_int(token); val >= 0) {
        m_ranges_.push_back({
            .lo = val,
            .hi = val,
        });
      }
    } else {
      auto left = token.substr(0, colon);
      auto right = token.substr(colon + 1);
      trim(left);
      trim(right);

      const int lo = left.empty() ? 0 : parse_int(left);
      const int hi = right.empty() ? max_phase : parse_int(right);

      if (lo >= 0 && hi >= 0) {
        m_ranges_.push_back({
            .lo = lo,
            .hi = hi,
        });
      }
    }
  }

  static constexpr void trim(std::string_view& sv) noexcept
  {
    while (!sv.empty() && sv.front() == ' ') {
      sv.remove_prefix(1);
    }
    while (!sv.empty() && sv.back() == ' ') {
      sv.remove_suffix(1);
    }
  }

  [[nodiscard]] static int parse_int(std::string_view sv) noexcept
  {
    int value = -1;
    // std::from_chars requires raw begin/end pointers; the "end" pointer is
    // one-past-the-last element and is only compared, never dereferenced.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::from_chars(sv.data(), sv.data() + sv.size(), value);
    return value;
  }
};

}  // namespace gtopt
