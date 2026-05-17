/**
 * @file      json_canonicalize.hpp
 * @brief     Rewrite alias keys in JSON text to canonical gtopt names
 * @date      2026-05-17
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Sits in front of `daw::json::from_json<Planning>(...)` and rewrites
 * alternative input key names (PyPSA `p_nom`, pandapower `max_p_mw`,
 * PLEXOS `Max Capacity`, PSR SDDP `GerMax`, PLP `PotMax`, gtopt-modern
 * `marginal_cost`, …) to the canonical name (e.g. `pmax`) expected by
 * the strict daw::json contract.
 *
 * Rationale (docs/analysis/naming-conventions.md §10.4):
 *
 *  * `daw::json` binds every contract member to a single literal name
 *    at compile time and gtopt uses `StrictParsePolicy`
 *    (`UseExactMappingsByDefault::yes`), so an alias key in raw JSON
 *    is rejected before any post-parse hook gets a chance to fold it.
 *  * Walking the JSON token-by-token before parsing lets us rewrite
 *    only the keys, never the values — safe even when a value string
 *    happens to equal an alias name (e.g. `"description": "max_demand
 *    is deprecated"`).
 *
 * The rewriter consumes the (alias → canonical) map from
 * `NamesRegistry::instance()`.  Aliases must be globally unique
 * across element types (the registry enforces this at load time), so
 * the rewriter does not need element-type context.
 */

#pragma once

#include <string>
#include <string_view>

namespace gtopt
{

class NamesRegistry;

/// Rewrite every `"alias":` object-key occurrence in `json_text` to
/// its canonical form using `registry`.  Returns a new string; the
/// input is not modified.
///
/// Performance: single linear pass over the input, O(N) in text size
/// plus O(K) per key lookup where K is the longest registered alias.
/// On the juan IPLP case (~12 MB JSON) the empirical overhead is
/// well under 100 ms.
///
/// Safety: only quoted strings that appear in object-key position
/// (immediately after `{` or `,` inside an object, ending in `:`) are
/// considered for substitution.  Quoted strings in value position are
/// passed through verbatim.  JSON string escapes (`\"`, `\\`, `\n`,
/// `\u00XX`, etc.) are recognised so escaped quotes inside strings do
/// not terminate the token scan.
[[nodiscard]] std::string canonicalize_json_keys(std::string_view json_text,
                                                 const NamesRegistry& registry);

/// Convenience overload using the process-wide singleton.
[[nodiscard]] std::string canonicalize_json_keys(std::string_view json_text);

}  // namespace gtopt
