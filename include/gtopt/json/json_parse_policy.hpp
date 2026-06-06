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
///
/// ``IEEE754Precise::yes`` is mandatory, not optional: daw-json-link's
/// default (fast) real parser silently mis-parses number literals with
/// more significant digits than fit in a ``uint64_t`` (> ~19) by orders
/// of magnitude when the document is fed as a ``std::string`` — which is
/// exactly how ``parse_planning_json`` feeds the canonicalised buffer.
/// The strtod fallback that handles over-long significands is compiled
/// out unless this flag is set.  See
/// https://github.com/beached/daw_json_link/issues/474 (daw 3.31.0).
constexpr auto StrictParsePolicy = daw::json::options::parse_flags<
    daw::json::options::PolicyCommentTypes::hash,
    daw::json::options::CheckedParseMode::yes,
    daw::json::options::ExcludeSpecialEscapes::yes,
    daw::json::options::IEEE754Precise::yes,
    daw::json::options::UseExactMappingsByDefault::yes>;

/// Per-member number options that allow ``Infinity`` / ``-Infinity``
/// (and ``NaN``) to round-trip through JSON as quoted strings.
///
/// daw-json-link's ``AllowNanInf`` mode requires ``LiteralAsStringOpt``
/// to be ``Maybe`` (or ``Always``), so the value is read as a quoted
/// literal like ``"Infinity"`` / ``"-Infinity"`` / ``"NaN"`` — see the
/// cookbook for details:
///   https://github.com/beached/daw_json_link/blob/release/docs/cookbook/member_options.md
///
/// **Usage** (per-field opt-in — daw-json-link does not provide a
/// global "allow inf" flag on the parse policy; the option is on the
/// member's number-options template parameter):
///
/// @code
/// json_number_null<"x", OptReal, gtopt::NumberOptsWithInf>
/// @endcode
///
/// **When this is needed**: only when a user-edited or third-party
/// JSON might carry a literal ``"Infinity"`` token.  plp2gtopt's own
/// writer prefers to *omit* unbounded fields entirely (see
/// ``gtopt_writer._sanitize_inf``), which gtopt's struct defaults treat
/// as ``+inf`` after the flatten-time clamp — so the round-trip is
/// already complete for our own outputs without this option being
/// applied.  The constant is here so adding inf-tolerance to a specific
/// field is one local edit, not a multi-file refactor.
[[maybe_unused]] constexpr auto NumberOptsWithInf =
    daw::json::number_opts_t::options(
        daw::json::options::LiteralAsStringOpt::Maybe,
        daw::json::options::JsonNumberErrors::AllowNanInf);

}  // namespace gtopt
