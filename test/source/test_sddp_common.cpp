// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_sddp_common.cpp
 * @brief     Lock-in tests for FormatSI and SDDPLogTag (sddp_common.hpp)
 * @date      2026-05-05
 * @copyright BSD-3-Clause
 *
 * Pin the rendered output of the lazy SI-suffix formatter and the lazy
 * SDDP log-prefix tag against future regressions:
 *
 *  - FormatSI buckets at 1e3 / 1e6 / 1e9 / 1e12 with the documented
 *    decimal counts (4 / 2 / 2 / 3 / 3) and sign placement.
 *  - SDDPLogTag's four overloads round-trip through both the implicit
 *    `operator std::string()` path (legacy callers) and the
 *    `std::formatter<SDDPLogTag>` path (`std::format("{}", ...)`).
 *  - The bracketed key matches the run_gtopt regex
 *    `r"SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:\s+a(\d+))?\]"`.
 */

#include <format>
#include <regex>
#include <string>

#include <doctest/doctest.h>
#include <gtopt/sddp_common.hpp>

using namespace gtopt;  // NOLINT(google-build-using-namespace)

// ─── A. FormatSI buckets ────────────────────────────────────────────────────

TEST_CASE("FormatSI: bucket boundaries and decimal counts")  // NOLINT
{
  SUBCASE("plain bucket below 1e3 keeps 4 decimals")
  {
    const std::string s = format_si(0.5);
    CHECK(s == "0.5000");
  }

  SUBCASE("kilo bucket [1e3, 1e6) uses K suffix and 2 decimals")
  {
    const std::string s = format_si(1234.5);
    CHECK(s == "1.23K");
  }

  SUBCASE("mega bucket [1e6, 1e9) uses M suffix and 2 decimals")
  {
    // 161.00e6 / 1e6 = 161.00 exactly.
    const std::string s = format_si(1.61e8);
    CHECK(s == "161.00M");
  }

  SUBCASE("giga bucket [1e9, 1e12) uses G suffix and 3 decimals")
  {
    const std::string s = format_si(1.234e+9);
    CHECK(s == "1.234G");
  }

  SUBCASE("tera bucket [1e12, ...) uses T suffix and 3 decimals")
  {
    const std::string s = format_si(1.5e+12);
    CHECK(s == "1.500T");
  }

  SUBCASE("negative values get a leading sign before the magnitude")
  {
    const std::string s = format_si(-1234.5);
    CHECK(s == "-1.23K");
  }

  SUBCASE("zero falls into the plain bucket")
  {
    const std::string s = format_si(0.0);
    CHECK(s == "0.0000");
  }
}

TEST_CASE(
    "FormatSI: implicit-string vs std::format produce identical output")  // NOLINT
{
  // Both consumer paths (legacy `std::string s = format_si(x);` and the
  // lazy `std::format("{}", format_si(x))`) MUST produce the exact same
  // characters — otherwise log-line consumers and structured tools
  // would see different renderings of the same value.
  for (const double v : {0.0, 0.5, 1234.5, -1234.5, 1.61e8, 1.234e+9, 1.5e+12})
  {
    const std::string via_implicit = format_si(v);
    const std::string via_format = std::format("{}", format_si(v));
    CHECK(via_implicit == via_format);
  }
}

// ─── B. SDDPLogTag overloads ────────────────────────────────────────────────

namespace
{
// Convenience constructors — `make_uid<Tag>` is the only sanctioned way
// to mint a `UidOf<Tag>` from a raw integer.  Use the fully-qualified
// `gtopt::uid_t` to avoid clashing with POSIX `uid_t` from <sys/types.h>.
[[nodiscard]] inline auto i_uid(gtopt::uid_t v) noexcept
{
  return make_uid<Iteration>(v);
}
[[nodiscard]] inline auto s_uid(gtopt::uid_t v) noexcept
{
  return make_uid<Scene>(v);
}
[[nodiscard]] inline auto p_uid(gtopt::uid_t v) noexcept
{
  return make_uid<Phase>(v);
}
}  // namespace

TEST_CASE(
    "SDDPLogTag: full (iter, scene, phase) tag formats correctly")  // NOLINT
{
  const auto tag = sddp_log("Forward", i_uid(1), s_uid(2), p_uid(3));

  SUBCASE("std::format path")
  {
    const auto s = std::format("{}", tag);
    CHECK(s == "SDDP Forward [i1 s2 p3]");
  }
  SUBCASE("implicit conversion path")
  {
    const std::string s = sddp_log("Forward", i_uid(1), s_uid(2), p_uid(3));
    CHECK(s == "SDDP Forward [i1 s2 p3]");
  }
}

TEST_CASE("SDDPLogTag: aperture overload appends a<uid>")  // NOLINT
{
  const auto tag = sddp_log("Aperture", i_uid(1), s_uid(2), p_uid(3), Uid {5});

  SUBCASE("std::format path")
  {
    const auto s = std::format("{}", tag);
    CHECK(s == "SDDP Aperture [i1 s2 p3 a5]");
  }
  SUBCASE("implicit conversion path")
  {
    const std::string s =
        sddp_log("Aperture", i_uid(1), s_uid(2), p_uid(3), Uid {5});
    CHECK(s == "SDDP Aperture [i1 s2 p3 a5]");
  }
}

TEST_CASE("SDDPLogTag: scene-only overload omits phase")  // NOLINT
{
  const auto tag = sddp_log("Forward", i_uid(1), s_uid(2));

  SUBCASE("std::format path")
  {
    const auto s = std::format("{}", tag);
    CHECK(s == "SDDP Forward [i1 s2]");
  }
  SUBCASE("implicit conversion path")
  {
    const std::string s = sddp_log("Forward", i_uid(1), s_uid(2));
    CHECK(s == "SDDP Forward [i1 s2]");
  }
}

TEST_CASE(
    "SDDPLogTag: iteration-only overload formats just [i<uid>]")  // NOLINT
{
  const auto tag = sddp_log("Init", i_uid(1));

  SUBCASE("std::format path")
  {
    const auto s = std::format("{}", tag);
    CHECK(s == "SDDP Init [i1]");
  }
  SUBCASE("implicit conversion path")
  {
    const std::string s = sddp_log("Init", i_uid(1));
    CHECK(s == "SDDP Init [i1]");
  }
}

TEST_CASE(
    "SDDPLogTag: implicit string == std::format for every overload")  // NOLINT
{
  // Lockguard: the formatter and the implicit conversion MUST agree on
  // every overload, so producers (info-level call sites) and consumers
  // (log parsers, run_gtopt regex) see identical strings regardless of
  // which path the call site happens to use.
  const auto via_implicit_full =
      std::string(sddp_log("Backward", i_uid(7), s_uid(8), p_uid(9)));
  const auto via_format_full =
      std::format("{}", sddp_log("Backward", i_uid(7), s_uid(8), p_uid(9)));
  CHECK(via_implicit_full == via_format_full);

  const auto via_implicit_aperture =
      std::string(sddp_log("Aperture", i_uid(7), s_uid(8), p_uid(9), Uid {42}));
  const auto via_format_aperture = std::format(
      "{}", sddp_log("Aperture", i_uid(7), s_uid(8), p_uid(9), Uid {42}));
  CHECK(via_implicit_aperture == via_format_aperture);

  const auto via_implicit_scene =
      std::string(sddp_log("Forward", i_uid(7), s_uid(8)));
  const auto via_format_scene =
      std::format("{}", sddp_log("Forward", i_uid(7), s_uid(8)));
  CHECK(via_implicit_scene == via_format_scene);

  const auto via_implicit_iter = std::string(sddp_log("Init", i_uid(7)));
  const auto via_format_iter = std::format("{}", sddp_log("Init", i_uid(7)));
  CHECK(via_implicit_iter == via_format_iter);
}

TEST_CASE("SDDPLogTag: rendered string matches run_gtopt regex")  // NOLINT
{
  // `run_gtopt` (Python) parses these prefixes with:
  //   re.compile(r"SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:\s+a(\d+))?\]")
  // Mirror that with std::regex; if either the formatter or the
  // implicit conversion ever drifts, the regex match fails here.
  const std::regex re {R"(SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:\s+a(\d+))?\])"};

  SUBCASE("phase-level prefix matches with no aperture group")
  {
    const auto rendered =
        std::format("{}", sddp_log("Forward", i_uid(11), s_uid(22), p_uid(33)));
    std::smatch m;
    REQUIRE(std::regex_search(rendered, m, re));
    CHECK(m[1].str() == "Forward");
    CHECK(m[2].str() == "11");
    CHECK(m[3].str() == "22");
    CHECK(m[4].str() == "33");
    CHECK(m[5].str().empty());  // optional aperture group not captured
  }

  SUBCASE("aperture-level prefix populates the optional a<uid> group")
  {
    const auto rendered = std::format(
        "{}", sddp_log("Aperture", i_uid(11), s_uid(22), p_uid(33), Uid {44}));
    std::smatch m;
    REQUIRE(std::regex_search(rendered, m, re));
    CHECK(m[1].str() == "Aperture");
    CHECK(m[2].str() == "11");
    CHECK(m[3].str() == "22");
    CHECK(m[4].str() == "33");
    CHECK(m[5].str() == "44");
  }
}
