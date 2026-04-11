// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      uid.hpp
 * @brief     Passkey-gated strong UID type `UidOf<Tag>` + `make_uid` factory
 * @date      2026-04-11
 * @copyright BSD-3-Clause
 *
 * ## Why this exists
 *
 * Historically a UID in gtopt was just `int32_t`, and strong UID aliases like
 * `BlockUid`, `StageUid`, `PhaseUid`, … were `strong::type<int32_t, Tag>`
 * which *did* allow brace-construction from a raw integer: `BlockUid{3}`
 * compiles anywhere.  This let test fixtures and production code synthesize
 * UIDs from loop counters — a well-known class of bug, because a UID is an
 * identity token, not a counter, and the N-th row in an array is not in
 * general the element with UID N.
 *
 * `UidOf<Tag>` closes that loophole.  It has no public constructor taking a
 * `uid_t`: the only way a user can materialize one from an integer is via
 * the `make_uid<Tag>(uid_t)` factory, and that factory holds a *passkey*
 * that nothing else in the codebase can construct.  At file/JSON boundaries
 * (where UIDs genuinely cross from raw integers into the type system) code
 * calls `make_uid<Block>(parsed_int)`.  Everywhere else, UIDs are copied
 * from existing elements — the compiler rejects fresh synthesis.
 *
 * ### What still works
 *
 * - **Default construction** → produces an "unknown" UID
 *   (`unknown_uid_of<Tag>()` is the named form).
 * - **Copy / move / compare** — `UidOf<Tag>` is `regular` and ordered.
 * - **Hashing** — usable as a map key.
 * - **Formatting** — usable with `std::format` / spdlog.
 * - **Reading the underlying integer** — `u.value()` or implicit conversion
 *   to `uid_t` (kept implicit so existing equality comparisons against
 *   raw integers in tests still compile).
 *
 * ### What is blocked
 *
 * - `UidOf<Block>{3}` — no matching constructor, hard compile error.
 * - `UidOf<Block>(3)` — same, no matching constructor.
 * - `static_cast<UidOf<Block>>(3)` — same, no conversion path.
 * - Cross-tag assignment: `UidOf<Block>{stage_uid}` — distinct types.
 *
 * ### What Phase A does *not* touch
 *
 * This header is purely additive.  `BlockUid`, `StageUid`, `PhaseUid`,
 * `SceneUid`, `ScenarioUid` are still `StrongUidType<Tag>` in their entity
 * headers — they will be flipped to `UidOf<Tag>` in Phase B.  Index strong
 * types (`BlockIndex`, `RowIndex`, …) are intentionally out of scope and
 * stay exactly as they are.
 */

#pragma once

#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <type_traits>

#include <gtopt/basic_types.hpp>

namespace gtopt
{

/// Passkey used to gate the `UidOf<Tag>` value constructor.
///
/// Nothing in user code can construct a `uid_passkey` — its default
/// constructor is private and the only friend is `make_uid<Tag>`.  That
/// makes `make_uid` the single sanctioned entry point for materializing
/// a `UidOf<Tag>` from a raw `uid_t`.
class uid_passkey
{
public:
  uid_passkey(const uid_passkey&) = default;
  uid_passkey(uid_passkey&&) = default;
  auto operator=(const uid_passkey&) -> uid_passkey& = default;
  auto operator=(uid_passkey&&) -> uid_passkey& = default;
  ~uid_passkey() = default;

private:
  constexpr uid_passkey() noexcept = default;

  // The factory is the only friend allowed to mint a passkey.
  template<class Tag>
  // NOLINTNEXTLINE(readability-redundant-declaration)
  friend constexpr auto make_uid(uid_t value) noexcept;
};

/// Strong, passkey-gated UID type.
///
/// @tparam Tag The entity type used as a compile-time tag
///             (e.g. `struct Block`, `struct Scenario`).  Two `UidOf`
///             instantiations with different tags are distinct types.
template<class Tag>
class UidOf
{
public:
  using tag_type = Tag;
  using underlying_type = uid_t;

  /// Default-constructs to the "unknown" sentinel value.
  constexpr UidOf() noexcept = default;

  /// Passkey-gated value constructor.  Callable only from `make_uid<Tag>`.
  constexpr UidOf(uid_t value, uid_passkey /*key*/) noexcept
      : m_value_(value)
  {
  }

  /// Read the raw underlying integer.
  [[nodiscard]] constexpr auto value() const noexcept -> uid_t
  {
    return m_value_;
  }

  /// Implicit conversion to `uid_t` — preserved so that existing
  /// equality comparisons against raw integer literals in tests and
  /// log output (`spdlog`, `std::format`) keep working.
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  constexpr operator uid_t() const noexcept { return m_value_; }

  [[nodiscard]] constexpr auto is_unknown() const noexcept -> bool
  {
    return m_value_ == unknown_uid;
  }

  // ── Regular, ordered, equality-comparable via defaulted operators ──

  friend constexpr auto operator==(const UidOf&, const UidOf&) noexcept
      -> bool = default;

  friend constexpr auto operator<=>(const UidOf&, const UidOf&) noexcept
      -> std::strong_ordering = default;

private:
  uid_t m_value_ {unknown_uid};
};

/// Sanctioned factory: construct a `UidOf<Tag>` from a raw `uid_t`.
///
/// Intended call sites are file / JSON boundaries where a raw integer
/// crosses from external data into the type system.  Inside the codebase,
/// prefer copying an existing UID from an element rather than calling this.
///
/// @note Templated on `Tag` (not deduced), so call as `make_uid<Block>(3)`.
template<class Tag>
[[nodiscard]] constexpr auto make_uid(uid_t value) noexcept
{
  return UidOf<Tag> {value, uid_passkey {}};
}

/// Named form of the default-constructed / "unknown" UID state.
/// Equivalent to `UidOf<Tag>{}`, but more explicit at call sites.
template<class Tag>
[[nodiscard]] constexpr auto unknown_uid_of() noexcept -> UidOf<Tag>
{
  return UidOf<Tag> {};
}

}  // namespace gtopt

// ─── std::hash specialization ───────────────────────────────────────────────

template<class Tag>
struct std::hash<gtopt::UidOf<Tag>>  // NOLINT(cert-dcl58-cpp)
{
  [[nodiscard]] constexpr auto operator()(
      const gtopt::UidOf<Tag>& u) const noexcept -> std::size_t
  {
    return std::hash<gtopt::uid_t> {}(u.value());
  }
};

// ─── std::formatter specialization ──────────────────────────────────────────
//
// Delegates to the uid_t formatter so that `std::format("{}", block_uid)`
// renders as the underlying integer — matching spdlog / format output of
// the existing strong-type family.

template<class Tag, class CharT>
struct std::formatter<gtopt::UidOf<Tag>, CharT>  // NOLINT(cert-dcl58-cpp)
    : std::formatter<gtopt::uid_t, CharT>
{
  template<class FormatContext>
  auto format(const gtopt::UidOf<Tag>& u, FormatContext& ctx) const
  {
    return std::formatter<gtopt::uid_t, CharT>::format(u.value(), ctx);
  }
};
