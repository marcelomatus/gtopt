/**
 * @file      json_schema.cpp
 * @brief     Implementation of the JSON Schema dump for the Planning model
 * @date      2026-06-08
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * The schema is generated at compile time from the daw_json_link
 * ``json_data_contract`` mappings via ``daw::json::to_json_schema<T>``.
 *
 * Renderable top-level types
 * --------------------------
 * Only ``PlanningOptions`` is exposed.  The full ``Planning`` (and the
 * ``System`` it embeds) cannot be rendered by the bundled daw_json_link:
 * the element-level field schedules map to ``json_variant_type_list``s
 * whose alternatives include *bare* scalar types (``double``, ``int``,
 * ``std::string``).  daw's ``to_json_schema`` variant handler accesses
 * ``JsonElement::expected_type`` for every alternative, which does not
 * exist on raw scalars — so ``to_json_schema<Planning>`` /
 * ``<System>`` fail to *compile*.  Exposing ``PlanningOptions`` (the
 * configuration/option contract, which uses only string/number/bool/
 * nested-class members) gives operators a useful, fully renderable
 * schema for the part of the input model they hand-edit most.
 */

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Instantiating `daw::json::to_json_schema` exercises the
// `apply_policy_flags` helper in daw_to_json.h, whose `if constexpr`
// branch refers to `json_details::serialization::set_bits` — an
// expression GCC 15's eager template-body checking (`-Wtemplate-body`)
// rejects as ill-formed even on the never-taken branch (an upstream
// daw_json_link issue).  Silence `-Wtemplate-body` for this whole
// translation unit; no other gtopt code instantiates the schema
// generator, so the suppression is fully local to this file.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic ignored "-Wtemplate-body"
#endif

#include <daw/json/daw_json_schema.h>
#include <gtopt/json/json_options.hpp>
#include <gtopt/json_schema.hpp>

namespace gtopt
{

namespace
{

/// Lowercase a name for case-insensitive matching of the requested type.
[[nodiscard]] std::string to_lower(std::string_view sv)
{
  std::string out;
  out.reserve(sv.size());
  std::ranges::transform(sv,
                         std::back_inserter(out),
                         [](unsigned char c)
                         { return static_cast<char>(std::tolower(c)); });
  return out;
}

/// A single dispatchable schema entry: the public name plus the generator
/// that renders that type's JSON Schema.
struct SchemaEntry
{
  std::string_view name;
  std::string (*generate)();
};

/// Ordered dispatch table of renderable top-level types.  Each generator
/// calls ``daw::json::to_json_schema<T>`` for a type that has a
/// ``json_data_contract`` and renders cleanly.  The first entry is the
/// default.  Only ``PlanningOptions`` is currently renderable (see the
/// file header for why ``Planning`` / ``System`` are not).
[[nodiscard]] const std::vector<SchemaEntry>& schema_entries()
{
  static const std::vector<SchemaEntry> entries {
      {
          .name = "options",
          .generate =
              []
          {
            return daw::json::to_json_schema<PlanningOptions>(
                "gtopt.PlanningOptions", "gtopt PlanningOptions input schema");
          },
      },
  };
  return entries;
}

}  // namespace

const std::vector<std::string_view>& known_schema_names()
{
  static const std::vector<std::string_view> names = []
  {
    std::vector<std::string_view> out;
    out.reserve(schema_entries().size());
    for (const auto& e : schema_entries()) {
      out.push_back(e.name);
    }
    return out;
  }();
  return names;
}

std::string dump_json_schema(std::string_view type_name)
{
  // Empty / absent name → the default schema (first entry).
  const std::string wanted = type_name.empty()
      ? std::string {schema_entries().front().name}
      : to_lower(type_name);

  for (const auto& entry : schema_entries()) {
    if (entry.name == wanted) {
      return entry.generate();
    }
  }

  // Unknown type: mirror --list-dialects' unknown-filter behaviour —
  // print an error listing the known names to stderr and signal failure
  // by returning an empty string.
  std::cerr << std::format(
      "ERROR: --json-schema: unknown type '{}'.  Known "
      "types: ",
      type_name);
  bool first = true;
  for (const auto& name : known_schema_names()) {
    std::cerr << (first ? "" : ", ") << name;
    first = false;
  }
  std::cerr << '\n';
  return {};
}

}  // namespace gtopt
