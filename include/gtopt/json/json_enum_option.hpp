/**
 * @file      json_enum_option.hpp
 * @brief     Generic JSON serialization helper for NamedEnum types
 * @date      2026-03-28
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Provides a single generic `enum_to_opt_name` template that replaces
 * per-enum overloads in JSON serialization files.
 */

#pragma once

#include <optional>
#include <string>

#include <gtopt/enum_option.hpp>
#include <gtopt/json/json_basic_types.hpp>

namespace daw::json::detail
{

/**
 * @brief Convert an optional NamedEnum value to an OptName for JSON output.
 *
 * Replaces the per-enum `enum_to_opt_name` overloads that were previously
 * duplicated in each JSON serialization header.
 */
template<gtopt::NamedEnum E>
inline OptName enum_to_opt_name(const std::optional<E>& e)
{
  return e ? OptName {std::string(gtopt::enum_name(*e))} : OptName {};
}

}  // namespace daw::json::detail
