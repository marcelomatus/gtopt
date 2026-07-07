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

using namespace gtopt;

// ─── A. FormatSI buckets ────────────────────────────────────────────────────

TEST_CASE("FormatSI: bucket boundaries and decimal counts")  // NOLINT
{
  // FormatSI uses a UNIFORM 6-decimal precision across every SI band
  // since the convergence-table rewrite (so UB / LB / α / gap columns
  // line up at the decimal point across magnitudes).  These pins guard
  // a regression back to the legacy per-bucket precision (4 / 2 / 2 /
  // 3 / 3 decimals).
  SUBCASE("plain bucket below 1e3 uses 6 decimals")
  {
    const std::string s = format_si(0.5);
    CHECK(s == "0.500000");
  }

  SUBCASE("kilo bucket [1e3, 1e6) uses K suffix and 6 decimals")
  {
    const std::string s = format_si(1234.5);
    CHECK(s == "1.234500K");
  }

  SUBCASE("mega bucket [1e6, 1e9) uses M suffix and 6 decimals")
  {
    // 161e6 / 1e6 = 161.000000 exactly.
    const std::string s = format_si(1.61e8);
    CHECK(s == "161.000000M");
  }

  SUBCASE("giga bucket [1e9, 1e12) uses G suffix and 6 decimals")
  {
    const std::string s = format_si(1.234e+9);
    CHECK(s == "1.234000G");
  }

  SUBCASE("tera bucket [1e12, ...) uses T suffix and 6 decimals")
  {
    const std::string s = format_si(1.5e+12);
    CHECK(s == "1.500000T");
  }

  SUBCASE("negative values get a leading sign before the magnitude")
  {
    const std::string s = format_si(-1234.5);
    CHECK(s == "-1.234500K");
  }

  SUBCASE("zero falls into the plain bucket")
  {
    const std::string s = format_si(0.0);
    CHECK(s == "0.000000");
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

// ─── PhaseGridRecorder ────────────────────────────────────────────────────
//
// Tests added 2026-05-07 alongside `bb05693b fix(phase_grid)` which
// switched the recorder's storage from a positional packed
// `std::string` (indexed by `value_of(phase_uid) - 1`, a UID-arithmetic
// category error) to a `std::map<PhaseUid, char>` keyed on the UID
// itself.  The cells string in the JSON output is now a UID-keyed
// object, not a positional string.

TEST_CASE("PhaseGridRecorder: empty by default")  // NOLINT
{
  PhaseGridRecorder recorder {};
  CHECK(recorder.empty());

  recorder.record(i_uid(1), s_uid(1), p_uid(1), GridCell::Forward);
  CHECK_FALSE(recorder.empty());
}

TEST_CASE("PhaseGridRecorder: priority semantics — std::max wins")  // NOLINT
{
  // The recorder applies `std::max` on the underlying char value of
  // `GridCell`.  The enum's char values determine the priority order:
  //   Idle ('.', 0x2E) < Aperture ('A', 0x41) < Backward ('B', 0x42)
  //   < Elastic ('E', 0x45) < Forward ('F', 0x46) < Infeasible ('X', 0x58)
  // So `Infeasible` is the absolute highest, `Forward` is higher than
  // `Backward`, and `Idle` is lowest.  Test the three branches:
  //   * higher-priority state wins on overwrite
  //   * lower-priority state does NOT overwrite a higher one
  //   * idle never wins
  PhaseGridRecorder recorder {};
  const auto iu = i_uid(1);
  const auto su = s_uid(1);
  const auto pu = p_uid(1);

  // Idle → Backward: higher wins.
  recorder.record(iu, su, pu, GridCell::Idle);
  recorder.record(iu, su, pu, GridCell::Backward);
  CHECK(recorder.to_json().contains(R"("1": "B")"));

  // Backward → Forward: 'F' > 'B' so Forward wins.
  recorder.record(iu, su, pu, GridCell::Forward);
  CHECK(recorder.to_json().contains(R"("1": "F")"));

  // Forward → Backward: 'B' < 'F' so Forward holds.
  recorder.record(iu, su, pu, GridCell::Backward);
  CHECK(recorder.to_json().contains(R"("1": "F")"));

  // Forward → Infeasible: 'X' is highest, must overwrite.
  recorder.record(iu, su, pu, GridCell::Infeasible);
  CHECK(recorder.to_json().contains(R"("1": "X")"));

  // Infeasible → Forward / Backward / Aperture / Elastic / Idle: none
  // overwrite — `Infeasible` is the absorbing state.
  recorder.record(iu, su, pu, GridCell::Forward);
  recorder.record(iu, su, pu, GridCell::Aperture);
  recorder.record(iu, su, pu, GridCell::Elastic);
  recorder.record(iu, su, pu, GridCell::Idle);
  CHECK(recorder.to_json().contains(R"("1": "X")"));
}

TEST_CASE(
    "PhaseGridRecorder: sparse PhaseUid layout emits only "
    "recorded UIDs (regression for bb05693b)")  // NOLINT
{
  // Pre-fix bug: `value_of(phase_uid) - 1` was used as a slot offset
  // into a packed cells string, so a sparse UID set silently produced
  // a positional string with '.' fillers in the unrecorded slots.
  // Post-fix storage is `std::map<PhaseUid, char>` and JSON emits
  // ONLY the UIDs actually recorded.
  PhaseGridRecorder recorder {};
  const auto iu = i_uid(1);
  const auto su = s_uid(1);

  recorder.record(iu, su, p_uid(3), GridCell::Forward);
  recorder.record(iu, su, p_uid(7), GridCell::Backward);

  const auto json = recorder.to_json();
  // Recorded UIDs present, ordered ascending by integer key.
  CHECK(json.contains(R"("3": "F")"));
  CHECK(json.contains(R"("7": "B")"));
  // Phantom slots between 3 and 7 must NOT appear (pre-fix would have
  // emitted "..F...B" with '.' fillers).
  CHECK_FALSE(json.contains(R"("4": ".")"));
  CHECK_FALSE(json.contains(R"("5": ".")"));
  CHECK_FALSE(json.contains(R"("6": ".")"));
  // No UID 1 / 2 either.
  CHECK_FALSE(json.contains(R"("1": ".")"));
  CHECK_FALSE(json.contains(R"("2": ".")"));

  // Sort order — "3" precedes "7" in the emitted JSON.
  const auto pos3 = json.find(R"("3": "F")");
  const auto pos7 = json.find(R"("7": "B")");
  REQUIRE(pos3 != std::string::npos);
  REQUIRE(pos7 != std::string::npos);
  CHECK(pos3 < pos7);
}

TEST_CASE(
    "PhaseGridRecorder: multiple (iter, scene) rows emit one "
    "JSON entry each")  // NOLINT
{
  PhaseGridRecorder recorder {};
  recorder.record(i_uid(1), s_uid(0), p_uid(1), GridCell::Forward);
  recorder.record(i_uid(1), s_uid(1), p_uid(1), GridCell::Forward);
  recorder.record(i_uid(2), s_uid(0), p_uid(1), GridCell::Backward);

  const auto json = recorder.to_json();
  // Three (iter, scene) keys → three row objects.
  std::size_t count = 0;
  for (std::size_t pos = 0;
       (pos = json.find(R"("i":)", pos)) != std::string::npos;
       ++pos)
  {
    ++count;
  }
  CHECK(count == 3);
  // Each row's iter/scene values are present.
  CHECK(json.contains(R"("i": 1)"));
  CHECK(json.contains(R"("i": 2)"));
  CHECK(json.contains(R"("s": 0)"));
  CHECK(json.contains(R"("s": 1)"));
}
