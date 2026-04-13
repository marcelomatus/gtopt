/**
 * @file      gtopt_json_io_parse_check.cpp
 * @brief     Exact-mapping from_json<Planning> instantiation for --check-json
 * @date      2026-04-12
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Isolates the ExactParsePolicy instantiation of from_json<Planning>
 * so it compiles in parallel with the main parse TU.  Used only by
 * the optional --check-json validation pass.
 */

#include <string_view>

#include <gtopt/json/json_planning.hpp>

namespace gtopt::detail
{

// NOLINTNEXTLINE(misc-use-internal-linkage) — called from _parse.cpp
Planning parse_planning_exact(std::string_view json)
{
  constexpr auto ExactParsePolicy = daw::json::options::parse_flags<
      daw::json::options::PolicyCommentTypes::hash,
      daw::json::options::CheckedParseMode::yes,
      daw::json::options::ExcludeSpecialEscapes::yes,
      daw::json::options::UseExactMappingsByDefault::yes>;

  return daw::json::from_json<Planning>(json, ExactParsePolicy);
}

}  // namespace gtopt::detail
