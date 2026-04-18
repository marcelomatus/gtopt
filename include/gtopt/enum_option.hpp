/**
 * @file      enum_option.hpp
 * @brief     Generic compile-time enum-to-string and string-to-enum framework
 * @date      2026-03-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a reusable pattern for mapping enum values to string names and
 * back.  Each enum defines a `std::to_array<EnumEntry<E>>` table and an
 * ADL `enum_entries(E)` customization point; the generic templates perform
 * the lookup.
 *
 * ### Defining a named enum
 * ```cpp
 * enum class Colour : uint8_t { red, green, blue };
 *
 * inline constexpr auto colour_entries = std::to_array<EnumEntry<Colour>>({
 *     {.name = "red",   .value = Colour::red},
 *     {.name = "green", .value = Colour::green},
 *     {.name = "blue",  .value = Colour::blue},
 * });
 *
 * [[nodiscard]] constexpr auto enum_entries(Colour) noexcept
 * {
 *   return std::span {colour_entries};
 * }
 * ```
 *
 * ### Using a named enum
 * ```cpp
 * auto c = enum_from_name<Colour>("green");   // std::optional<Colour>
 * auto n = enum_name(Colour::blue);           // std::string_view
 * ```
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gtopt
{

/**
 * @brief Name-value pair for an enumerator.
 *
 * With C++26 static reflection (P2996 `std::meta::enumerators_of`) this
 * table could be generated automatically.  Until then it is maintained
 * manually next to each enum definition.
 *
 * @note  Set @c is_alias to @c true for back-compat spellings that should
 * still parse but be hidden from the "(expected: …)" list produced by
 * @ref require_enum on error.  Aliases are still accepted by
 * @ref enum_from_name so existing configs keep working.
 */
template<typename E>
struct EnumEntry
{
  std::string_view name;
  E value;
  bool is_alias = false;
};

namespace detail
{
/**
 * @brief ASCII case-insensitive equality (no locale, constexpr-friendly).
 *
 * Folds [A-Z] to [a-z] on the fly and compares byte-for-byte.  Non-ASCII
 * bytes are compared literally, which is fine because every entry in the
 * project's enum tables is lowercase ASCII.
 */
[[nodiscard]] constexpr auto ascii_iequals(std::string_view a,
                                           std::string_view b) noexcept -> bool
{
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto ca = static_cast<unsigned char>(a[i]);
    auto cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') {
      ca = static_cast<unsigned char>(ca + ('a' - 'A'));
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = static_cast<unsigned char>(cb + ('a' - 'A'));
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}
}  // namespace detail

// ─── Low-level span-based lookup (explicit table) ───────────────────────────

/**
 * @brief Look up an enumerator by name (span overload).
 *
 * The comparison is ASCII case-insensitive so CLI / JSON / config callers
 * can accept ``"january"``, ``"January"``, ``"JANUARY"`` interchangeably.
 * Every entry in the project's enum tables is lowercase ASCII, so no
 * locale is involved.
 *
 * @tparam E  Enum type.
 * @tparam N  Table size (deduced from the span extent).
 * @param entries  Compile-time table of name-value pairs.
 * @param name     Name to search for (ASCII case-insensitive).
 * @return The matching enumerator, or @c std::nullopt if not found.
 */
template<typename E, std::size_t N>
[[nodiscard]] constexpr auto enum_from_name(
    std::span<const EnumEntry<E>, N> entries, std::string_view name) noexcept
    -> std::optional<E>
{
  const auto it =
      std::ranges::find_if(entries,
                           [name](const EnumEntry<E>& e)
                           { return detail::ascii_iequals(e.name, name); });
  if (it != entries.end()) {
    return it->value;
  }
  return std::nullopt;
}

/**
 * @brief Return the canonical name of an enumerator (span overload).
 *
 * @tparam E  Enum type.
 * @tparam N  Table size (deduced from the span extent).
 * @param entries  Compile-time table of name-value pairs.
 * @param value    The enumerator to look up.
 * @return The name string, or @c "unknown" for out-of-range values.
 */
template<typename E, std::size_t N>
[[nodiscard]] constexpr auto enum_name(std::span<const EnumEntry<E>, N> entries,
                                       E value) noexcept -> std::string_view
{
  const auto it = std::ranges::find_if(
      entries, [value](const EnumEntry<E>& e) { return e.value == value; });
  return it != entries.end() ? it->name : "unknown";
}

// ─── NamedEnum concept + ADL-based lookup ───────────────────────────────────

/**
 * @brief Concept for enums with an ADL `enum_entries(E)` customization point.
 *
 * Each named enum provides a free function in the same namespace:
 * ```cpp
 * [[nodiscard]] constexpr auto enum_entries(MyEnum) noexcept
 *     { return std::span{my_enum_entries}; }
 * ```
 */
template<typename E>
concept NamedEnum = requires(E e) {
  { enum_entries(e) };
};

/**
 * @brief Look up a NamedEnum value by name (ADL-based).
 *
 * Uses the ADL `enum_entries(E{})` customization point to obtain the
 * lookup table, then delegates to the span-based overload.
 *
 * @tparam E  A type satisfying the NamedEnum concept.
 * @param name  Name to search for (ASCII case-insensitive).
 * @return The matching enumerator, or @c std::nullopt if not found.
 */
template<NamedEnum E>
[[nodiscard]] constexpr auto enum_from_name(std::string_view name) noexcept
    -> std::optional<E>
{
  return enum_from_name(
      enum_entries(
          E {}),  // NOLINT(bugprone-invalid-enum-default-initialization)
      name);
}

/**
 * @brief Return the canonical name of a NamedEnum value (ADL-based).
 *
 * Uses the ADL `enum_entries(value)` customization point to obtain the
 * lookup table, then delegates to the span-based overload.
 *
 * @param value  The enumerator to look up.
 * @return The name string, or @c "unknown" for out-of-range values.
 */
template<NamedEnum E>
[[nodiscard]] constexpr auto enum_name(E value) noexcept -> std::string_view
{
  return enum_name(enum_entries(value), value);
}

/**
 * @brief Look up a NamedEnum value by name; throw on invalid input.
 *
 * Builds a user-friendly error message listing all valid names when the
 * lookup fails.  Intended for CLI / config-file option parsing where an
 * invalid value should be a hard error.
 *
 * @tparam E  A type satisfying the NamedEnum concept.
 * @param option_name  Human-readable option name (for the error message).
 * @param value        The string supplied by the user.
 * @return The matching enumerator.
 * @throws std::invalid_argument if @p value does not match any entry.
 */
template<NamedEnum E>
[[nodiscard]] auto require_enum(std::string_view option_name,
                                std::string_view value) -> E
{
  if (auto opt = enum_from_name<E>(value)) {
    return *opt;
  }
  const auto entries = enum_entries(
      E {});  // NOLINT(bugprone-invalid-enum-default-initialization)
  std::string valid;
  for (const auto& e : entries) {
    if (e.is_alias) {
      continue;
    }
    if (!valid.empty()) {
      valid += ", ";
    }
    valid += e.name;
  }
  throw std::invalid_argument(
      std::format("invalid value '{}' for option '{}' (expected: {})",
                  value,
                  option_name,
                  valid));
}

}  // namespace gtopt
