// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_uid_of.cpp
 * @brief     Tests for passkey-gated `UidOf<Tag>` + `make_uid` factory
 * @date      2026-04-11
 *
 * This is the Phase-A unit-test layer for the Uid-family refactor.  It
 * exercises `UidOf<Tag>` in isolation, using throwaway test tags, and
 * proves:
 *
 *  - default construction gives the "unknown" sentinel,
 *  - `make_uid<Tag>(n)` produces a UID whose value is `n`,
 *  - copy/equality/ordering are correct,
 *  - `std::hash` and `std::format` specializations work,
 *  - raw-integer construction (`UidOf<Tag>{3}`) is rejected at compile time,
 *  - cross-tag assignment is rejected at compile time,
 *  - the types are distinct per tag (no accidental interchangeability).
 *
 * Phase B will flip `BlockUid` / `StageUid` / `PhaseUid` / `SceneUid` /
 * `ScenarioUid` onto `UidOf<Tag>`; those tests live elsewhere and will be
 * updated alongside the flip.  This file deliberately does *not* reference
 * any of those existing aliases so that Phase A can land as a pure addition.
 */

#include <cstddef>
#include <format>
#include <string>
#include <type_traits>
#include <unordered_map>

#include <doctest/doctest.h>
#include <gtopt/block.hpp>
#include <gtopt/phase.hpp>
#include <gtopt/scenario.hpp>
#include <gtopt/stage.hpp>
#include <gtopt/uid.hpp>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

namespace  // NOLINT(cert-dcl59-cpp,fuchsia-header-anon-namespaces,google-build-namespaces,misc-anonymous-namespace-in-header)
{

// Throwaway tag types used only in this file.  Keeping them local avoids
// coupling Phase A to any existing entity header.
struct TagA
{
};
struct TagB
{
};

using UidA = UidOf<TagA>;
using UidB = UidOf<TagB>;

}  // namespace

// ─── Default construction / unknown sentinel ────────────────────────────────

TEST_CASE("UidOf default-constructs to the unknown sentinel")  // NOLINT
{
  const UidA u;
  CHECK(u.value() == unknown_uid);
  CHECK(u.is_unknown());
  CHECK(static_cast<gtopt::uid_t>(u) == unknown_uid);

  const auto named = unknown_uid_of<TagA>();
  CHECK(named.is_unknown());
  CHECK(named == u);
}

// ─── make_uid factory ───────────────────────────────────────────────────────

TEST_CASE("make_uid<Tag>(n) produces a UidOf<Tag> carrying n")  // NOLINT
{
  const auto u = make_uid<TagA>(42);
  CHECK(u.value() == 42);
  CHECK_FALSE(u.is_unknown());
  CHECK(static_cast<gtopt::uid_t>(u) == 42);

  // Round-trip through implicit conversion
  const gtopt::uid_t raw = u;
  CHECK(raw == 42);
}

TEST_CASE("make_uid is constexpr-callable")  // NOLINT
{
  constexpr auto u = make_uid<TagA>(7);
  static_assert(u.value() == 7);
  CHECK(u.value() == 7);
}

// ─── Regular / equality / ordering ──────────────────────────────────────────

TEST_CASE("UidOf equality compares by underlying value")  // NOLINT
{
  const auto a = make_uid<TagA>(5);
  const auto b = make_uid<TagA>(5);
  const auto c = make_uid<TagA>(6);

  CHECK(a == b);
  CHECK_FALSE(a == c);
  CHECK(a != c);

  // Copy preserves value
  const UidA copy = a;  // NOLINT(performance-unnecessary-copy-initialization)
  CHECK(copy == a);
  CHECK(copy.value() == 5);
}

TEST_CASE("UidOf is totally ordered via <=>")  // NOLINT
{
  const auto small = make_uid<TagA>(1);
  const auto large = make_uid<TagA>(99);

  CHECK(small < large);
  CHECK(large > small);
  CHECK(small <= small);
  CHECK(large >= large);
  CHECK((small <=> large) == std::strong_ordering::less);
  CHECK((large <=> small) == std::strong_ordering::greater);
  CHECK((small <=> small) == std::strong_ordering::equal);
}

// ─── std::hash / unordered_map key ──────────────────────────────────────────

TEST_CASE("UidOf is hashable and usable as an unordered_map key")  // NOLINT
{
  std::unordered_map<UidA, int> m;
  m[make_uid<TagA>(10)] = 100;
  m[make_uid<TagA>(20)] = 200;
  m[make_uid<TagA>(30)] = 300;

  CHECK(m.size() == 3);
  CHECK(m.at(make_uid<TagA>(10)) == 100);
  CHECK(m.at(make_uid<TagA>(20)) == 200);
  CHECK(m.at(make_uid<TagA>(30)) == 300);

  // Unknown key is absent
  CHECK(m.find(unknown_uid_of<TagA>()) == m.end());

  // Verify std::hash gives the same value as hashing the raw uid
  const std::hash<UidA> huid;
  const std::hash<gtopt::uid_t> hraw;
  CHECK(huid(make_uid<TagA>(42)) == hraw(42));
}

// ─── std::format ────────────────────────────────────────────────────────────

TEST_CASE("UidOf renders via std::format as its underlying integer")  // NOLINT
{
  const auto u = make_uid<TagA>(123);
  CHECK(std::format("{}", u) == "123");
  CHECK(std::format("{:05}", u) == "00123");

  const auto unk = unknown_uid_of<TagA>();
  CHECK(std::format("{}", unk) == std::format("{}", unknown_uid));
}

// ─── Compile-time prohibitions ──────────────────────────────────────────────

TEST_CASE("UidOf is not constructible from a raw uid_t")  // NOLINT
{
  // The whole point of the passkey is to block this.  If these static_asserts
  // ever start firing, the passkey gate has been breached.
  static_assert(!std::is_constructible_v<UidA, gtopt::uid_t>,
                "UidOf<Tag> must not be constructible from a raw uid_t");
  static_assert(!std::is_constructible_v<UidA, int>,
                "UidOf<Tag> must not be constructible from a raw int");
  static_assert(!std::is_convertible_v<gtopt::uid_t, UidA>,
                "uid_t must not implicitly convert to UidOf<Tag>");

  // `UidOf<Tag>{}` (default) must still work.
  static_assert(std::is_default_constructible_v<UidA>);

  // Copy and move must still work.
  static_assert(std::is_copy_constructible_v<UidA>);
  static_assert(std::is_move_constructible_v<UidA>);
  static_assert(std::is_copy_assignable_v<UidA>);

  CHECK(true);  // non-static reachability
}

TEST_CASE("UidOf<TagA> and UidOf<TagB> are distinct types")  // NOLINT
{
  static_assert(!std::is_same_v<UidA, UidB>);
  static_assert(!std::is_constructible_v<UidA, UidB>);
  static_assert(!std::is_constructible_v<UidB, UidA>);
  static_assert(!std::is_convertible_v<UidA, UidB>);
  static_assert(!std::is_convertible_v<UidB, UidA>);

  // Each still converts to uid_t (that's intentional — see header doc).
  static_assert(std::is_convertible_v<UidA, gtopt::uid_t>);
  static_assert(std::is_convertible_v<UidB, gtopt::uid_t>);

  CHECK(true);
}

TEST_CASE("uid_passkey cannot be constructed outside make_uid")  // NOLINT
{
  // The passkey's default constructor is private.  If user code could
  // construct one, it could bypass make_uid entirely.
  static_assert(!std::is_default_constructible_v<uid_passkey>,
                "uid_passkey default ctor must be inaccessible from user code");
  CHECK(true);
}

// ─── Implicit uid_t conversion still works ──────────────────────────────────

TEST_CASE("UidOf implicitly converts to uid_t for legacy interop")  // NOLINT
{
  // The implicit-to-uid_t conversion is deliberately kept so existing
  // equality comparisons against integer literals in tests and log
  // formatting keep working during the Phase-B rollout.
  const auto u = make_uid<TagA>(7);

  CHECK(u == 7);  // via implicit conversion to uid_t, then int comparison
  CHECK(7 == u);

  // Explicit cast also works
  CHECK(static_cast<gtopt::uid_t>(u) == 7);
  CHECK(u.value() == 7);
}

// ─── Concrete type aliases
// ────────────────────────────────────────────────────

TEST_CASE("Concrete alias types are distinct UidOf specialisations")  // NOLINT
{
  static_assert(!std::is_same_v<BlockUid, StageUid>);
  static_assert(!std::is_same_v<StageUid, ScenarioUid>);
  static_assert(!std::is_same_v<ScenarioUid, PhaseUid>);
  static_assert(!std::is_same_v<BlockUid, PhaseUid>);
}

TEST_CASE("Concrete alias construction and value")  // NOLINT
{
  const BlockUid bu = make_uid<Block>(3);
  const StageUid su = make_uid<Stage>(3);
  const ScenarioUid scu = make_uid<Scenario>(3);
  const PhaseUid pu = make_uid<Phase>(3);

  CHECK(bu.value() == 3);
  CHECK(su.value() == 3);
  CHECK(scu.value() == 3);
  CHECK(pu.value() == 3);
}

TEST_CASE("value_of free function unwraps a UidOf")  // NOLINT
{
  const StageUid su = make_uid<Stage>(7);
  CHECK(value_of(su) == 7);
  const BlockUid bu = make_uid<Block>(42);
  CHECK(value_of(bu) == 42);
}

TEST_CASE("unknown_uid_of<Tag> matches default-constructed UidOf")  // NOLINT
{
  const BlockUid unk = unknown_uid_of<Block>();
  CHECK(unk.is_unknown());
  CHECK(unk == BlockUid {});
}

TEST_CASE("BlockUid as unordered_map key")  // NOLINT
{
  std::unordered_map<BlockUid, std::string> m;
  const BlockUid k1 = make_uid<Block>(10);
  const BlockUid k2 = make_uid<Block>(20);
  m[k1] = "ten";
  m[k2] = "twenty";

  CHECK(m.at(k1) == "ten");
  CHECK(m.at(k2) == "twenty");
  CHECK(m.count(make_uid<Block>(10)) == 1);
  CHECK(m.count(make_uid<Block>(99)) == 0);
}

TEST_CASE("UidOf std::format — concrete types render as integers")  // NOLINT
{
  const BlockUid bu = make_uid<Block>(17);
  const std::string s = std::format("{}", bu);
  CHECK(s == "17");
}
