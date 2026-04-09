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
 * constexpr auto enum_entries(Colour) noexcept
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
 */
template<typename E>
struct EnumEntry
{
  std::string_view name;
  E value;
};

// ─── Low-level span-based lookup (explicit table) ───────────────────────────

/**
 * @brief Look up an enumerator by its canonical name (span overload).
 *
 * @tparam E  Enum type.
 * @tparam N  Table size (deduced from the span extent).
 * @param entries  Compile-time table of name-value pairs.
 * @param name     Case-sensitive name to search for.
 * @return The matching enumerator, or @c std::nullopt if not found.
 */
template<typename E, std::size_t N>
[[nodiscard]] constexpr auto enum_from_name(
    std::span<const EnumEntry<E>, N> entries, std::string_view name) noexcept
    -> std::optional<E>
{
  const auto it = std::ranges::find_if(
      entries, [name](const EnumEntry<E>& e) { return e.name == name; });
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
 * constexpr auto enum_entries(MyEnum) noexcept
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
 * @param name  Case-sensitive name to search for.
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
  const auto entries = enum_entries(E {});
  std::string valid;
  for (const auto& e : entries) {
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
