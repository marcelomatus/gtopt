// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_asciify.cpp
 * @brief     Unit tests for the `asciify` / `asciify_into` primitives
 *            (issue #508 Phase 0).
 *
 * Validates the branchless LUT-based ASCIIfier: every byte in
 * `[A-Za-z0-9_]` survives verbatim; every other byte (including
 * multi-byte UTF-8 continuations) is mapped to `_`.  No run collapsing
 * — a multi-byte UTF-8 separator yields a multi-underscore run, which
 * keeps the mapping bijective-ish and prevents collisions on similar
 * names.
 */

#include <string>
#include <string_view>

#include <doctest/doctest.h>
#include <gtopt/as_label.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

TEST_CASE("asciify preserves every byte in the LP-name-safe set")
{
  SUBCASE("ASCII letters survive verbatim, case is preserved")
  {
    CHECK(asciify("AbCdEf") == "AbCdEf");
    CHECK(asciify("XYZxyz") == "XYZxyz");
    // Boundaries of the LUT-allowed ranges
    CHECK(asciify("Aa0Zz9") == "Aa0Zz9");
  }
  SUBCASE("Digits survive verbatim")
  {
    CHECK(asciify("0123456789") == "0123456789");
  }
  SUBCASE("Underscore survives verbatim, runs are NOT collapsed")
  {
    CHECK(asciify("a_b") == "a_b");
    CHECK(asciify("____") == "____");
    CHECK(asciify("a__b___c") == "a__b___c");
  }
}

TEST_CASE("asciify replaces every disallowed byte with '_'")
{
  SUBCASE("ASCII punctuation maps to '_'")
  {
    CHECK(asciify("a b") == "a_b");
    CHECK(asciify("a-b") == "a_b");
    CHECK(asciify("a.b") == "a_b");
    CHECK(asciify("a@b") == "a_b");
    CHECK(asciify("a#b") == "a_b");
    CHECK(asciify("a/b") == "a_b");
    CHECK(asciify("a:b") == "a_b");
  }
  SUBCASE("Multi-byte UTF-8 yields a run of underscores (one per byte)")
  {
    // Right-arrow → is U+2192, encoded as 3 UTF-8 bytes (0xE2 0x86 0x92)
    CHECK(asciify("Jadresic220_II→MonteMina220")
          == "Jadresic220_II___MonteMina220");
    // Greek small letter alpha α is 2 UTF-8 bytes (0xCE 0xB1)
    CHECK(asciify("nodo_α") == "nodo___");
  }
  SUBCASE("Worked examples from the design doc §4.3")
  {
    CHECK(asciify("MyLine") == "MyLine");
    CHECK(asciify("Diesel #2") == "Diesel__2");
    CHECK(asciify("Pangue@2026") == "Pangue_2026");
    CHECK(asciify("") == "");
  }
}

TEST_CASE("asciify_into appends to an existing buffer")
{
  SUBCASE("Empty buffer behaves like asciify")
  {
    std::string out;
    asciify_into(out, "AbCd");
    CHECK(out == "AbCd");
  }
  SUBCASE("Non-empty buffer is preserved and appended to")
  {
    std::string out {"prefix_"};
    asciify_into(out, "Diesel #2");
    CHECK(out == "prefix_Diesel__2");
  }
  SUBCASE("Multiple appends share one buffer (arena pattern)")
  {
    // Mimics `AsciiNameCache::populate_()`: write multiple asciified
    // names into one shared arena, recording each view's [off, len).
    std::string arena;
    arena.reserve(64);
    const auto a_off = arena.size();
    asciify_into(arena, "Jadresic220_II→MonteMina220");
    const std::string_view a_view {arena.data() + a_off, arena.size() - a_off};
    const auto b_off = arena.size();
    asciify_into(arena, "Pangue@2026");
    const std::string_view b_view {arena.data() + b_off, arena.size() - b_off};
    CHECK(a_view == "Jadresic220_II___MonteMina220");
    CHECK(b_view == "Pangue_2026");
  }
}

TEST_CASE("asciify output length always equals input length")
{
  // Hard contract used by AsciiNameCache's two-pass populate: pass 1
  // sums input byte sizes and reserves the arena to exactly that
  // capacity, so pass 2's writes must never reallocate.  This test
  // pins the length-preservation invariant down — every byte either
  // stays itself or becomes a single underscore.
  constexpr std::string_view samples[] = {
      "ASCIIonly",
      "with spaces and tabs\t!",
      "Multi-Byte: αβγ→",
      "_____",
      "",
      "a",
      "1",
  };
  for (auto sv : samples) {
    CHECK(asciify(sv).size() == sv.size());
  }
}
