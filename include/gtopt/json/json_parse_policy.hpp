/**
 * @file      json_parse_policy.hpp
 * @brief     Shared daw::json parse policies for gtopt
 * @date      2026-04-13
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Centralises the daw::json parse-flag combinations used across
 * production code and the unit-test suite.  All policies are strict
 * (`UseExactMappingsByDefault::yes`) — unknown JSON fields are
 * rejected with a descriptive diagnostic instead of being silently
 * ignored.  This catches typos and schema drift (e.g. renamed fields)
 * early, both for users and for the test fixtures that exercise the
 * parser.
 *
 * @code
 * // Production / tests:
 * auto planning = daw::json::from_json<Planning>(
 *     json_string, gtopt::StrictParsePolicy);
 * @endcode
 */

#pragma once

#include <daw/json/daw_json_link.h>

namespace gtopt
{

/// Strict parser with hash-comment support and full checking.
/// Rejects unknown JSON members.  Use for all JSON input — user-facing
/// planning files and internally constructed overlays (e.g. --set) alike.
constexpr auto StrictParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::UseExactMappingsByDefault::yes>;

}  // namespace gtopt
